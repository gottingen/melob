//
// Copyright (C) 2024 EA group inc.
// Author: Jeff.li lijippy@163.com
// All rights reserved.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
//

#include <melon/naming/sns_naming_service.h>
#include <thread>
#include <melon/fiber/fiber.h>
#include <melon/rpc/channel.h>
#include <melon/rpc/controller.h>
#include <melon/utility/third_party/rapidjson/document.h>
#include <melon/utility/third_party/rapidjson/memorybuffer.h>
#include <melon/utility/third_party/rapidjson/writer.h>
#include <melon/utility/string_splitter.h>
#include <turbo/flags/flag.h>
#include <turbo/container/flat_hash_set.h>

namespace melon::naming {
    static turbo::flat_hash_set<std::string> sns_status_set = {"1", "2", "3", "4"};
}  // namespace melon::naming
TURBO_FLAG(std::string, sns_server, "", "The address of sns api").on_validate(turbo::AllPassValidator<std::string>::validate);
TURBO_FLAG(int, sns_timeout_ms, 3000, "Timeout for discovery requests");
TURBO_FLAG(std::string, sns_status, "1",
           "Status of services. 1 for normal, 2 for slow, 3 for full, 4 for dead").on_validate(
        [](std::string_view value, std::string *err) noexcept -> bool {
            if (melon::naming::sns_status_set.find(value) == melon::naming::sns_status_set.end()) {
                *err = "Invalid status, should be 1, 2, 3 or 4";
                return false;
            }
            return true;
        });

TURBO_FLAG(int, sns_renew_interval_s, 30, "The interval between two consecutive renews").on_validate(
        turbo::GtValidator<int, 5>::validate);
TURBO_FLAG(int, sns_reregister_threshold, 3, "The renew error threshold beyond"
                                             " which Register would be called again");

namespace melon::naming {

    static inline melon::PeerStatus to_peer_status(const std::string &status) {
        if (status == "1") {
            return melon::PeerStatus::NORMAL;
        } else if (status == "2") {
            return melon::PeerStatus::SLOW;
        } else if (status == "3") {
            return melon::PeerStatus::FULL;
        } else {
            return melon::PeerStatus::DEAD;
        }
    }

    static turbo::Status make_sns_channel(Channel *channel) {
        ChannelOptions channel_options;
        channel_options.protocol = PROTOCOL_MELON_STD;
        channel_options.timeout_ms = turbo::get_flag(FLAGS_sns_timeout_ms);
        channel_options.connect_timeout_ms = turbo::get_flag(FLAGS_sns_timeout_ms) / 3;
        if (channel->Init(turbo::get_flag(FLAGS_sns_server).c_str(), "rr", &channel_options) != 0) {
            LOG(ERROR) << "Fail to init channel to " << turbo::get_flag(FLAGS_sns_server);
            return turbo::internal_error("Fail to init channel");
        }
        return turbo::OkStatus();
    }

    static bool is_valid(const melon::SnsPeer &peer) {
        return peer.has_app_name() && !peer.app_name().empty() &&
               peer.has_zone() && !peer.zone().empty() &&
               peer.has_servlet_name() && !peer.servlet_name().empty() &&
               peer.has_env() && !peer.env().empty() &&
               peer.has_color() && !peer.color().empty() &&
               peer.has_address() && !peer.address().empty();
    }


    turbo::Status
    SnsNamingParams::register_service(const std::string &service_name, const std::shared_ptr<melon::SnsRequest> &req) {
        // check request
        if(service_name.empty()) {
            return turbo::invalid_argument_error("service_name is empty");
        }
        STATUS_RETURN_IF_ERROR(check_service_name(req.get()));
        std::unique_lock lock(_mutex);
        auto it = _services.find(service_name);
        if (it != _services.end()) {
            return turbo::invalid_argument_error("service already exists");
        }
        _services[service_name] = req;
        return turbo::OkStatus();
    }

    turbo::Status SnsNamingParams::check_service_name(turbo::Nonnull<const melon::SnsRequest *> req) const {
        if (!req->has_app_name()) {
            return turbo::invalid_argument_error("app_name is empty");
        }

        if (req->zones().empty()) {
            return turbo::invalid_argument_error("zones is empty");
        }

        if (req->env().empty()) {
            return turbo::invalid_argument_error("envs is empty");
        }

        if (req->color().empty()) {
            return turbo::invalid_argument_error("colors is empty");
        }
        return turbo::OkStatus();
    }

    turbo::Status SnsNamingParams::update_service(const std::string &service_name, const std::shared_ptr<melon::SnsRequest> &req) {
        if(service_name.empty()) {
            return turbo::invalid_argument_error("service_name is empty");
        }
        STATUS_RETURN_IF_ERROR(check_service_name(req.get()));
        std::unique_lock lock(_mutex);
        _services[service_name] = req;
        return turbo::OkStatus();
    }

    std::shared_ptr<melon::SnsRequest> SnsNamingParams::get_service(std::string_view service_name) {
        std::shared_lock lock(_mutex);
        auto it = _services.find(service_name);
        if (it == _services.end()) {
            return nullptr;
        }
        return it->second;
    }

    SnsNamingClient::SnsNamingClient()
            : _th(INVALID_FIBER), _registered(false) {
    }

    SnsNamingClient::~SnsNamingClient() {
        if (_registered.load(mutil::memory_order_acquire)) {
            fiber_stop(_th);
            fiber_join(_th, nullptr);
            do_cancel();
        }
    }

    int SnsNamingClient::register_peer(const melon::SnsPeer &params) {
        if (_registered.load(mutil::memory_order_relaxed) ||
            _registered.exchange(true, mutil::memory_order_release)) {
            return 0;
        }
        if (!is_valid(params)) {
            return -1;
        }
        _params = params;

        if (do_register() != 0) {
            return -1;
        }
        if (fiber_start_background(&_th, nullptr, periodic_renew, this) != 0) {
            LOG(ERROR) << "Fail to start background PeriodicRenew";
            return -1;
        }
        return 0;
    }

    int SnsNamingClient::do_register() {
        Channel chan;
        if(!make_sns_channel(&chan).ok()) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }
        Controller cntl;
        melon::SnsService_Stub stub(&chan);
        melon::SnsResponse response;
        stub.registry(&cntl, &_params, &response, nullptr);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to register peer: " << cntl.ErrorText();
            return -1;
        }
        if (response.errcode() != melon::Errno::OK && response.errcode() != melon::Errno::AlreadyExists) {
            LOG(ERROR) << "Fail to register peer: " << response.errmsg();
            return -1;
        }
        _current_discovery_server = cntl.remote_side();
        return 0;
    }

    int SnsNamingClient::do_renew() const {
        Channel chan;
        if(!make_sns_channel(&chan).ok()) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }

        Controller cntl;
        melon::SnsService_Stub stub(&chan);
        melon::SnsResponse response;
        auto request = _params;
        request.set_status(to_peer_status(turbo::get_flag(FLAGS_sns_status)));
        stub.update(&cntl, &request, &response, nullptr);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to register peer: " << cntl.ErrorText();
            return -1;
        }
        if (response.errcode() != melon::Errno::OK) {
            LOG(ERROR) << "Fail to register peer: " << response.errmsg();
            return -1;
        }
        return 0;
    }

    int SnsNamingClient::do_cancel() const {
        Channel chan;
        if(!make_sns_channel(&chan).ok()) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }
        Controller cntl;
        melon::SnsService_Stub stub(&chan);
        melon::SnsResponse response;
        stub.cancel(&cntl, &_params, &response, nullptr);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to register peer: " << cntl.ErrorText();
            return -1;
        }
        if (response.errcode() != melon::Errno::OK) {
            LOG(ERROR) << "Fail to register peer: " << response.errmsg();
            return -1;
        }
        return 0;
    }

    void *SnsNamingClient::periodic_renew(void *arg) {
        auto *sns = static_cast<SnsNamingClient *>(arg);
        int consecutive_renew_error = 0;
        int64_t init_sleep_s = turbo::get_flag(FLAGS_sns_renew_interval_s) / 2 +
                               mutil::fast_rand_less_than(turbo::get_flag(FLAGS_sns_renew_interval_s) / 2);
        if (fiber_usleep(init_sleep_s * 1000000) != 0) {
            if (errno == ESTOP) {
                return nullptr;
            }
        }

        while (!fiber_stopped(fiber_self())) {
            if (consecutive_renew_error == turbo::get_flag(FLAGS_sns_reregister_threshold)) {
                LOG(WARNING) << "Re-register since discovery renew error threshold reached";
                // Do register until succeed or Cancel is called
                while (!fiber_stopped(fiber_self())) {
                    if (sns->do_register() == 0) {
                        break;
                    }
                    fiber_usleep(turbo::get_flag(FLAGS_sns_renew_interval_s) * 1000000);
                }
                consecutive_renew_error = 0;
            }
            if (sns->do_renew() != 0) {
                consecutive_renew_error++;
                continue;
            }
            consecutive_renew_error = 0;
            fiber_usleep(turbo::get_flag(FLAGS_sns_renew_interval_s) * 1000000);
        }
        return nullptr;
    }

    int SnsNamingService::GetServers(const char *service_name,
                                     std::vector<ServerNode> *servers) {
        if (service_name == nullptr || *service_name == '\0') {
            LOG_FIRST_N(ERROR, 1) << "Invalid parameters";
            return -1;
        }

        auto req = SnsNamingParams::get_instance()->get_service(service_name);
        if (req == nullptr) {
            LOG(ERROR) << "Service not found: " << service_name;
            return -1;
        }
        Channel chan;
        if(!make_sns_channel(&chan).ok()) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }

        Controller cntl;
        melon::SnsService_Stub stub(&chan);
        melon::SnsResponse response;

        stub.naming(&cntl, req.get(), &response, nullptr);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to register peer: " << cntl.ErrorText();
            return -1;
        }
        if (response.errcode() != melon::Errno::OK) {
            LOG(ERROR) << "Fail to register peer: " << response.errmsg();
            return -1;
        }

        for (int i = 0; i < response.servlets_size(); ++i) {
            const auto &peer = response.servlets(i);
            if (!is_valid(peer)) {
                LOG(ERROR) << "Invalid peer: " << peer.DebugString();
                continue;
            }
            ServerNode node;
            if (mutil::str2endpoint(peer.address().c_str(), &node.addr) != 0) {
                LOG(ERROR) << "Invalid address: " << peer.address();
                continue;
            }
            node.tag = peer.app_name() + "." + peer.zone() + "." + peer.env() + "." + peer.color();
            servers->push_back(node);
        }
        return 0;
    }

    void SnsNamingService::Describe(std::ostream &os,
                                    const DescribeOptions &) const {
        os << "sns";
        return;
    }

    NamingService *SnsNamingService::New() const {
        return new SnsNamingService;
    }

    void SnsNamingService::Destroy() {
        delete this;
    }


}  // namespace melon::naming
