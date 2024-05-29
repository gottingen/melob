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



#include <gflags/gflags.h>
#include <string>                                       // std::string
#include <set>                                          // std::set
#include <melon/utility/string_printf.h>
#include <melon/utility/third_party/rapidjson/document.h>
#include <melon/utility/third_party/rapidjson/stringbuffer.h>
#include <melon/utility/third_party/rapidjson/prettywriter.h>
#include <melon/utility/time/time.h>
#include <melon/fiber/fiber.h>
#include <melon/rpc/log.h>
#include <melon/rpc/channel.h>
#include <melon/naming/file_naming_service.h>
#include <melon/naming/consul_naming_service.h>


namespace melon::naming {

    DEFINE_string(consul_agent_addr, "http://127.0.0.1:8500",
                  "The query string of request consul for discovering service.");
    DEFINE_string(consul_service_discovery_url,
                  "/v1/health/service/",
                  "The url of consul for discovering service.");
    DEFINE_string(consul_url_parameter, "?stale&passing",
                  "The query string of request consul for discovering service.");
    DEFINE_int32(consul_connect_timeout_ms, 200,
                 "Timeout for creating connections to consul in milliseconds");
    DEFINE_int32(consul_blocking_query_wait_secs, 60,
                 "Maximum duration for the blocking request in secs.");
    DEFINE_bool(consul_enable_degrade_to_file_naming_service, false,
                "Use local backup file when consul cannot connect");
    DEFINE_string(consul_file_naming_service_dir, "",
                  "When it degraded to file naming service, the file with name of the "
                  "service name will be searched in this dir to use.");
    DEFINE_int32(consul_retry_interval_ms, 500,
                 "Wait so many milliseconds before retry when error happens");

    constexpr char kConsulIndex[] = "X-Consul-Index";

    std::string RapidjsonValueToString(const MUTIL_RAPIDJSON_NAMESPACE::Value &value) {
        MUTIL_RAPIDJSON_NAMESPACE::StringBuffer buffer;
        MUTIL_RAPIDJSON_NAMESPACE::PrettyWriter<MUTIL_RAPIDJSON_NAMESPACE::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

    int ConsulNamingService::DegradeToOtherServiceIfNeeded(const char *service_name,
                                                           std::vector<ServerNode> *servers) {
        if (FLAGS_consul_enable_degrade_to_file_naming_service && !_backup_file_loaded) {
            _backup_file_loaded = true;
            const std::string file(FLAGS_consul_file_naming_service_dir + service_name);
            LOG(INFO) << "Load server list from " << file;
            FileNamingService fns;
            return fns.GetServers(file.c_str(), servers);
        }
        return -1;
    }

    int ConsulNamingService::GetServers(const char *service_name,
                                        std::vector<ServerNode> *servers) {
        if (!_consul_connected) {
            ChannelOptions opt;
            opt.protocol = PROTOCOL_HTTP;
            opt.connect_timeout_ms = FLAGS_consul_connect_timeout_ms;
            opt.timeout_ms = (FLAGS_consul_blocking_query_wait_secs + 10) * mutil::Time::kMillisecondsPerSecond;
            if (_channel.Init(FLAGS_consul_agent_addr.c_str(), "rr", &opt) != 0) {
                LOG(ERROR) << "Fail to init channel to consul at " << FLAGS_consul_agent_addr;
                return DegradeToOtherServiceIfNeeded(service_name, servers);
            }
            _consul_connected = true;
        }

        if (_consul_url.empty()) {
            _consul_url.append(FLAGS_consul_service_discovery_url);
            _consul_url.append(service_name);
            _consul_url.append(FLAGS_consul_url_parameter);
        }

        servers->clear();
        std::string consul_url(_consul_url);
        if (!_consul_index.empty()) {
            mutil::string_appendf(&consul_url, "&index=%s&wait=%ds", _consul_index.c_str(),
                                  FLAGS_consul_blocking_query_wait_secs);
        }

        Controller cntl;
        cntl.http_request().uri() = consul_url;
        _channel.CallMethod(NULL, &cntl, NULL, NULL, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Fail to access " << consul_url << ": "
                       << cntl.ErrorText();
            return DegradeToOtherServiceIfNeeded(service_name, servers);
        }

        const std::string *index = cntl.http_response().GetHeader(kConsulIndex);
        if (index != nullptr) {
            if (*index == _consul_index) {
                LOG_EVERY_N(INFO, 100) << "There is no service changed for the list of "
                                       << service_name
                                       << ", consul_index: " << _consul_index;
                return -1;
            }
        } else {
            LOG(ERROR) << "Failed to parse consul index of " << service_name << ".";
            return -1;
        }

        // Sort/unique the inserted vector is faster, but may have a different order
        // of addresses from the file. To make assertions in tests easier, we use
        // set to de-duplicate and keep the order.
        std::set<ServerNode> presence;

        MUTIL_RAPIDJSON_NAMESPACE::Document services;
        services.Parse(cntl.response_attachment().to_string().c_str());
        if (!services.IsArray()) {
            LOG(ERROR) << "The consul's response for "
                       << service_name << " is not a json array";
            return -1;
        }

        for (MUTIL_RAPIDJSON_NAMESPACE::SizeType i = 0; i < services.Size(); ++i) {
            auto itr_service = services[i].FindMember("Service");
            if (itr_service == services[i].MemberEnd()) {
                LOG(ERROR) << "No service info in node: "
                           << RapidjsonValueToString(services[i]);
                continue;
            }

            const MUTIL_RAPIDJSON_NAMESPACE::Value &service = itr_service->value;
            auto itr_address = service.FindMember("Address");
            auto itr_port = service.FindMember("Port");
            if (itr_address == service.MemberEnd() ||
                !itr_address->value.IsString() ||
                itr_port == service.MemberEnd() ||
                !itr_port->value.IsUint()) {
                LOG(ERROR) << "Service with no valid address or port: "
                           << RapidjsonValueToString(service);
                continue;
            }

            mutil::EndPoint end_point;
            if (str2endpoint(service["Address"].GetString(),
                             service["Port"].GetUint(),
                             &end_point) != 0) {
                LOG(ERROR) << "Service with illegal address or port: "
                           << RapidjsonValueToString(service);
                continue;
            }

            ServerNode node;
            node.addr = end_point;
            auto itr_tags = service.FindMember("Tags");
            if (itr_tags != service.MemberEnd()) {
                if (itr_tags->value.IsArray()) {
                    if (itr_tags->value.Size() > 0) {
                        // Tags in consul is an array, here we only use the first one.
                        const MUTIL_RAPIDJSON_NAMESPACE::Value &tag = itr_tags->value[0];
                        if (tag.IsString()) {
                            node.tag = tag.GetString();
                        } else {
                            LOG(ERROR) << "First tag returned by consul is not string, service: "
                                       << RapidjsonValueToString(service);
                            continue;
                        }
                    }
                } else {
                    LOG(ERROR) << "Service tags returned by consul is not json array, service: "
                               << RapidjsonValueToString(service);
                    continue;
                }
            }

            if (presence.insert(node).second) {
                servers->push_back(node);
            } else {
                RPC_VLOG << "Duplicated server=" << node;
            }
        }

        _consul_index = *index;

        if (servers->empty() && !services.Empty()) {
            LOG(ERROR) << "All service about " << service_name
                       << " from consul is invalid, refuse to update servers";
            return -1;
        }

        RPC_VLOG << "Got " << servers->size()
                 << (servers->size() > 1 ? " servers" : " server")
                 << " from " << service_name;
        return 0;
    }

    int ConsulNamingService::RunNamingService(const char *service_name,
                                              NamingServiceActions *actions) {
        std::vector<ServerNode> servers;
        bool ever_reset = false;
        for (;;) {
            servers.clear();
            const int rc = GetServers(service_name, &servers);
            // If `fiber_stop' is called to stop the ns fiber when `melon::Join‘ is called
            // in `GetServers' to wait for a rpc to complete. The fiber will be woken up,
            // reset `TaskMeta::interrupted' and continue to join the rpc. After the rpc is complete,
            // `fiber_usleep' will not sense the interrupt signal and sleep successfully.
            // Finally, the ns fiber will never exit. So need to check the stop status of
            // the fiber here and exit the fiber in time.
            if (fiber_stopped(fiber_self())) {
                RPC_VLOG << "Quit NamingServiceThread=" << fiber_self();
                return 0;
            }
            if (rc == 0) {
                ever_reset = true;
                actions->ResetServers(servers);
            } else {
                if (!ever_reset) {
                    // ResetServers must be called at first time even if GetServers
                    // failed, to wake up callers to `WaitForFirstBatchOfServers'
                    ever_reset = true;
                    servers.clear();
                    actions->ResetServers(servers);
                }
                if (fiber_usleep(
                        std::max(FLAGS_consul_retry_interval_ms, 1) * mutil::Time::kMicrosecondsPerMillisecond) < 0) {
                    if (errno == ESTOP) {
                        RPC_VLOG << "Quit NamingServiceThread=" << fiber_self();
                        return 0;
                    }
                    PLOG(FATAL) << "Fail to sleep";
                    return -1;
                }
            }
        }
        CHECK(false);
        return -1;
    }


    void ConsulNamingService::Describe(std::ostream &os,
                                       const DescribeOptions &) const {
        os << "consul";
    }

    NamingService *ConsulNamingService::New() const {
        return new ConsulNamingService;
    }

    void ConsulNamingService::Destroy() {
        delete this;
    }

} // namespace melon::naming