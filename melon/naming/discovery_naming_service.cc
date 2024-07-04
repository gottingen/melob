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



#include <turbo/flags/flag.h>
#include <melon/utility/third_party/rapidjson/document.h>
#include <melon/utility/third_party/rapidjson/memorybuffer.h>
#include <melon/utility/third_party/rapidjson/writer.h>
#include <melon/utility/string_printf.h>
#include <melon/utility/strings/string_split.h>
#include <melon/utility/fast_rand.h>
#include <melon/fiber/fiber.h>
#include <melon/rpc/channel.h>
#include <melon/rpc/controller.h>
#include <melon/naming/discovery_naming_service.h>
#include <turbo/strings/str_split.h>

#ifdef BILIBILI_INTERNAL
# define DEFAULT_DISCOVERY_API_ADDR "http://api.bilibili.co/discovery/nodes"
#else
# define DEFAULT_DISCOVERY_API_ADDR ""
#endif

TURBO_FLAG(std::string, discovery_api_addr, DEFAULT_DISCOVERY_API_ADDR, "The address of discovery api");
TURBO_FLAG(int, discovery_timeout_ms, 3000, "Timeout for discovery requests");
TURBO_FLAG(std::string,discovery_env, "prod", "Environment of services");
TURBO_FLAG(std::string,discovery_status, "1", "Status of services. 1 for ready, 2 for not ready, 3 for all");
TURBO_FLAG(std::string,discovery_zone, "", "Zone of services");
TURBO_FLAG(int, discovery_renew_interval_s, 30, "The interval between two consecutive renews");
TURBO_FLAG(int, discovery_reregister_threshold, 3, "The renew error threshold beyond"
                                                " which Register would be called again");

namespace melon::naming {

    static pthread_once_t s_init_discovery_channel_once = PTHREAD_ONCE_INIT;
    static Channel *s_discovery_channel = NULL;

    static int ListDiscoveryNodes(const char *discovery_api_addr, std::string *servers) {
        Channel api_channel;
        ChannelOptions channel_options;
        channel_options.protocol = PROTOCOL_HTTP;
        channel_options.timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms);
        channel_options.connect_timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms) / 3;
        if (api_channel.Init(discovery_api_addr, "", &channel_options) != 0) {
            LOG(FATAL) << "Fail to init channel to " << discovery_api_addr;
            return -1;
        }
        Controller cntl;
        cntl.http_request().uri() = discovery_api_addr;
        api_channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(FATAL) << "Fail to access " << cntl.http_request().uri()
                       << ": " << cntl.ErrorText();
            return -1;
        }

        servers->assign("list://");

        const std::string response = cntl.response_attachment().to_string();
        MUTIL_RAPIDJSON_NAMESPACE::Document d;
        d.Parse(response.c_str());
        if (!d.IsObject()) {
            LOG(ERROR) << "Fail to parse " << response << " as json object";
            return -1;
        }
        auto itr = d.FindMember("data");
        if (itr == d.MemberEnd()) {
            LOG(ERROR) << "No data field in discovery nodes response";
            return -1;
        }
        const MUTIL_RAPIDJSON_NAMESPACE::Value &data = itr->value;
        if (!data.IsArray()) {
            LOG(ERROR) << "data field is not an array";
            return -1;
        }
        for (MUTIL_RAPIDJSON_NAMESPACE::SizeType i = 0; i < data.Size(); ++i) {
            const MUTIL_RAPIDJSON_NAMESPACE::Value &addr_item = data[i];
            auto itr_addr = addr_item.FindMember("addr");
            auto itr_status = addr_item.FindMember("status");
            if (itr_addr == addr_item.MemberEnd() ||
                !itr_addr->value.IsString() ||
                itr_status == addr_item.MemberEnd() ||
                !itr_status->value.IsUint() ||
                itr_status->value.GetUint() != 0) {
                continue;
            }
            servers->push_back(',');
            servers->append(itr_addr->value.GetString(),
                            itr_addr->value.GetStringLength());
        }
        return 0;
    }

    static void NewDiscoveryChannel() {
        // NOTE: Newly added discovery server is NOT detected until this server
        // is restarted. The reasons for this design is that NS cluster rarely
        // changes. Although we could detect new discovery servers by implmenenting
        // a NamingService, however which is too heavy for solving such a rare case.
        std::string discovery_servers;
        if (ListDiscoveryNodes(turbo::get_flag(FLAGS_discovery_api_addr).c_str(), &discovery_servers) != 0) {
            LOG(ERROR) << "Fail to get discovery nodes from " << turbo::get_flag(FLAGS_discovery_api_addr);
            return;
        }
        ChannelOptions channel_options;
        channel_options.protocol = PROTOCOL_HTTP;
        channel_options.timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms);
        channel_options.connect_timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms) / 3;
        s_discovery_channel = new Channel;
        if (s_discovery_channel->Init(discovery_servers.c_str(), "rr", &channel_options) != 0) {
            LOG(ERROR) << "Fail to init channel to " << discovery_servers;
            return;
        }
    }

    inline Channel *GetOrNewDiscoveryChannel() {
        pthread_once(&s_init_discovery_channel_once, NewDiscoveryChannel);
        return s_discovery_channel;
    }

    bool DiscoveryRegisterParam::IsValid() const {
        return !appid.empty() && !hostname.empty() && !addrs.empty() &&
               !env.empty() && !zone.empty() && !version.empty();
    }

    DiscoveryClient::DiscoveryClient()
            : _th(INVALID_FIBER), _registered(false) {}

    DiscoveryClient::~DiscoveryClient() {
        if (_registered.load(std::memory_order_acquire)) {
            fiber_stop(_th);
            fiber_join(_th, NULL);
            DoCancel();
        }
    }

    static int ParseCommonResult(const mutil::IOBuf &buf, std::string *error_text) {
        const std::string s = buf.to_string();
        MUTIL_RAPIDJSON_NAMESPACE::Document d;
        d.Parse(s.c_str());
        if (!d.IsObject()) {
            LOG(ERROR) << "Fail to parse " << buf << " as json object";
            return -1;
        }
        auto itr_code = d.FindMember("code");
        if (itr_code == d.MemberEnd() || !itr_code->value.IsInt()) {
            LOG(ERROR) << "Invalid `code' field in " << buf;
            return -1;
        }
        int code = itr_code->value.GetInt();
        auto itr_message = d.FindMember("message");
        if (itr_message != d.MemberEnd() && itr_message->value.IsString() && error_text) {
            error_text->assign(itr_message->value.GetString(),
                               itr_message->value.GetStringLength());
        }
        return code;
    }

    int DiscoveryClient::DoRenew() const {
        // May create short connections which are OK.
        ChannelOptions channel_options;
        channel_options.protocol = PROTOCOL_HTTP;
        channel_options.timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms);
        channel_options.connect_timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms) / 3;
        Channel chan;
        if (chan.Init(_current_discovery_server, &channel_options) != 0) {
            LOG(FATAL) << "Fail to init channel to " << _current_discovery_server;
            return -1;
        }

        Controller cntl;
        cntl.http_request().set_method(HTTP_METHOD_POST);
        cntl.http_request().uri() = "/discovery/renew";
        cntl.http_request().set_content_type("application/x-www-form-urlencoded");
        mutil::IOBufBuilder os;
        os << "appid=" << _params.appid
           << "&hostname=" << _params.hostname
           << "&env=" << _params.env
           << "&region=" << _params.region
           << "&zone=" << _params.zone;
        os.move_to(cntl.request_attachment());
        chan.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to post /discovery/renew: " << cntl.ErrorText();
            return -1;
        }
        std::string error_text;
        if (ParseCommonResult(cntl.response_attachment(), &error_text) != 0) {
            LOG(ERROR) << "Fail to renew " << _params.hostname << " to " << _params.appid
                       << ": " << error_text;
            return -1;
        }
        return 0;
    }

    void *DiscoveryClient::PeriodicRenew(void *arg) {
        DiscoveryClient *d = static_cast<DiscoveryClient *>(arg);
        int consecutive_renew_error = 0;
        int64_t init_sleep_s = turbo::get_flag(FLAGS_discovery_renew_interval_s) / 2 +
                               mutil::fast_rand_less_than(turbo::get_flag(FLAGS_discovery_renew_interval_s) / 2);
        if (fiber_usleep(init_sleep_s * 1000000) != 0) {
            if (errno == ESTOP) {
                return NULL;
            }
        }

        while (!fiber_stopped(fiber_self())) {
            if (consecutive_renew_error == turbo::get_flag(FLAGS_discovery_reregister_threshold)) {
                LOG(WARNING) << "Re-register since discovery renew error threshold reached";
                // Do register until succeed or Cancel is called
                while (!fiber_stopped(fiber_self())) {
                    if (d->DoRegister() == 0) {
                        break;
                    }
                    fiber_usleep(turbo::get_flag(FLAGS_discovery_renew_interval_s) * 1000000);
                }
                consecutive_renew_error = 0;
            }
            if (d->DoRenew() != 0) {
                consecutive_renew_error++;
                continue;
            }
            consecutive_renew_error = 0;
            fiber_usleep(turbo::get_flag(FLAGS_discovery_renew_interval_s) * 1000000);
        }
        return NULL;
    }

    int DiscoveryClient::Register(const DiscoveryRegisterParam &params) {
        if (_registered.load(std::memory_order_relaxed) ||
            _registered.exchange(true, std::memory_order_release)) {
            return 0;
        }
        if (!params.IsValid()) {
            return -1;
        }
        _params = params;

        if (DoRegister() != 0) {
            return -1;
        }
        if (fiber_start_background(&_th, NULL, PeriodicRenew, this) != 0) {
            LOG(ERROR) << "Fail to start background PeriodicRenew";
            return -1;
        }
        return 0;
    }

    int DiscoveryClient::DoRegister() {
        Channel *chan = GetOrNewDiscoveryChannel();
        if (NULL == chan) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }
        Controller cntl;
        cntl.http_request().set_method(HTTP_METHOD_POST);
        cntl.http_request().uri() = "/discovery/register";
        cntl.http_request().set_content_type("application/x-www-form-urlencoded");
        mutil::IOBufBuilder os;
        os << "appid=" << _params.appid
           << "&hostname=" << _params.hostname;

        std::vector<std::string_view> addrs = turbo::str_split(_params.addrs, ',');
        for (size_t i = 0; i < addrs.size(); ++i) {
            if (!addrs[i].empty()) {
                os << "&addrs=" << addrs[i];
            }
        }

        os << "&env=" << _params.env
           << "&zone=" << _params.zone
           << "&region=" << _params.region
           << "&status=" << _params.status
           << "&version=" << _params.version
           << "&metadata=" << _params.metadata;
        os.move_to(cntl.request_attachment());
        chan->CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to register " << _params.appid << ": " << cntl.ErrorText();
            return -1;
        }
        std::string error_text;
        if (ParseCommonResult(cntl.response_attachment(), &error_text) != 0) {
            LOG(ERROR) << "Fail to register " << _params.hostname << " to " << _params.appid
                       << ": " << error_text;
            return -1;
        }
        _current_discovery_server = cntl.remote_side();
        return 0;
    }

    int DiscoveryClient::DoCancel() const {
        // May create short connections which are OK.
        ChannelOptions channel_options;
        channel_options.protocol = PROTOCOL_HTTP;
        channel_options.timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms);
        channel_options.connect_timeout_ms = turbo::get_flag(FLAGS_discovery_timeout_ms) / 3;
        Channel chan;
        if (chan.Init(_current_discovery_server, &channel_options) != 0) {
            LOG(FATAL) << "Fail to init channel to " << _current_discovery_server;
            return -1;
        }

        Controller cntl;
        cntl.http_request().set_method(HTTP_METHOD_POST);
        cntl.http_request().uri() = "/discovery/cancel";
        cntl.http_request().set_content_type("application/x-www-form-urlencoded");
        mutil::IOBufBuilder os;
        os << "appid=" << _params.appid
           << "&hostname=" << _params.hostname
           << "&env=" << _params.env
           << "&region=" << _params.region
           << "&zone=" << _params.zone;
        os.move_to(cntl.request_attachment());
        chan.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to post /discovery/cancel: " << cntl.ErrorText();
            return -1;
        }
        std::string error_text;
        if (ParseCommonResult(cntl.response_attachment(), &error_text) != 0) {
            LOG(ERROR) << "Fail to cancel " << _params.hostname << " in " << _params.appid
                       << ": " << error_text;
            return -1;
        }
        return 0;
    }

    // ========== DiscoveryNamingService =============

    int DiscoveryNamingService::GetServers(const char *service_name,
                                           std::vector<ServerNode> *servers) {
        if (service_name == NULL || *service_name == '\0' ||
                turbo::get_flag(FLAGS_discovery_env).empty() ||
                turbo::get_flag(FLAGS_discovery_status).empty()) {
            LOG_FIRST_N(ERROR, 1) << "Invalid parameters";
            return -1;
        }
        Channel *chan = GetOrNewDiscoveryChannel();
        if (NULL == chan) {
            LOG(ERROR) << "Fail to create discovery channel";
            return -1;
        }
        servers->clear();
        Controller cntl;
        std::string uri_str = mutil::string_printf(
                "/discovery/fetchs?appid=%s&env=%s&status=%s", service_name,
                turbo::get_flag(FLAGS_discovery_env).c_str(), turbo::get_flag(FLAGS_discovery_status).c_str());
        if (!turbo::get_flag(FLAGS_discovery_zone).empty()) {
            uri_str.append("&zone=");
            uri_str.append(turbo::get_flag(FLAGS_discovery_zone));
        }
        cntl.http_request().uri() = uri_str;
        chan->CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to get /discovery/fetchs: " << cntl.ErrorText();
            return -1;
        }

        const std::string response = cntl.response_attachment().to_string();
        MUTIL_RAPIDJSON_NAMESPACE::Document d;
        d.Parse(response.c_str());
        if (!d.IsObject()) {
            LOG(ERROR) << "Fail to parse " << response << " as json object";
            return -1;
        }
        auto itr_data = d.FindMember("data");
        if (itr_data == d.MemberEnd()) {
            LOG(ERROR) << "No data field in discovery/fetchs response";
            return -1;
        }
        const MUTIL_RAPIDJSON_NAMESPACE::Value &data = itr_data->value;
        auto itr_service = data.FindMember(service_name);
        if (itr_service == data.MemberEnd()) {
            LOG(ERROR) << "No " << service_name << " field in discovery response";
            return -1;
        }
        const MUTIL_RAPIDJSON_NAMESPACE::Value &services = itr_service->value;
        auto itr_instances = services.FindMember("instances");
        if (itr_instances == services.MemberEnd()) {
            LOG(ERROR) << "Fail to find instances";
            return -1;
        }
        const MUTIL_RAPIDJSON_NAMESPACE::Value &instances = itr_instances->value;
        if (!instances.IsArray()) {
            LOG(ERROR) << "Fail to parse instances as an array";
            return -1;
        }

        for (MUTIL_RAPIDJSON_NAMESPACE::SizeType i = 0; i < instances.Size(); ++i) {
            std::string metadata;
            // convert metadata in object to string
            auto itr_metadata = instances[i].FindMember("metadata");
            if (itr_metadata != instances[i].MemberEnd()) {
                MUTIL_RAPIDJSON_NAMESPACE::MemoryBuffer buffer;
                MUTIL_RAPIDJSON_NAMESPACE::Writer<MUTIL_RAPIDJSON_NAMESPACE::MemoryBuffer> writer(buffer);
                itr_metadata->value.Accept(writer);
                metadata.assign(buffer.GetBuffer(), buffer.GetSize());
            }

            auto itr = instances[i].FindMember("addrs");
            if (itr == instances[i].MemberEnd() || !itr->value.IsArray()) {
                LOG(ERROR) << "Fail to find addrs or addrs is not an array";
                return -1;
            }
            const MUTIL_RAPIDJSON_NAMESPACE::Value &addrs = itr->value;
            for (MUTIL_RAPIDJSON_NAMESPACE::SizeType j = 0; j < addrs.Size(); ++j) {
                if (!addrs[j].IsString()) {
                    continue;
                }
                // The result returned by discovery include protocol prefix, such as
                // http://172.22.35.68:6686, which should be removed.
                std::string_view addr(addrs[j].GetString(), addrs[j].GetStringLength());
                std::string_view::size_type pos = addr.find("://");
                if (pos != std::string_view::npos) {
                    if (pos != 4 /* sizeof("grpc") */ ||
                        strncmp("grpc", addr.data(), 4) != 0) {
                        // Skip server that has prefix but not start with "grpc"
                        continue;
                    }
                    addr.remove_prefix(pos + 3);
                }
                ServerNode node;
                node.tag = metadata;
                // Variable addr contains data from addrs[j].GetString(), it is a
                // null-terminated string, so it is safe to pass addr.data() as the
                // first parameter to str2endpoint.
                if (str2endpoint(addr.data(), &node.addr) != 0) {
                    LOG(ERROR) << "Invalid address=`" << addr << '\'';
                    continue;
                }
                servers->push_back(node);
            }
        }
        return 0;
    }

    void DiscoveryNamingService::Describe(std::ostream &os,
                                          const DescribeOptions &) const {
        os << "discovery";
        return;
    }

    NamingService *DiscoveryNamingService::New() const {
        return new DiscoveryNamingService;
    }

    void DiscoveryNamingService::Destroy() {
        delete this;
    }


} // namespace melon::naming
