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

#pragma once

#include <melon/proto/rpc/sns.pb.h>
#include <melon/naming/periodic_naming_service.h>
#include <melon/rpc/channel.h>
#include <turbo/container/flat_hash_map.h>
#include <shared_mutex>
#include <turbo/utility/status.h>

namespace melon::naming {

    class SnsNamingClient {
    public:
        SnsNamingClient();

        ~SnsNamingClient();

        int register_peer(const melon::SnsPeer &req);

    private:
        static void *periodic_renew(void *arg);

        int do_cancel() const;

        int do_register();

        int do_renew() const;

    private:
        fiber_t _th;
        std::atomic<bool> _registered;
        melon::SnsPeer _params;
        mutil::EndPoint _current_discovery_server;

    };

    class SnsRequestBuilder {
    public:
        SnsRequestBuilder() : request(std::make_shared<melon::SnsRequest>()) {}

        SnsRequestBuilder&set_app_name(const std::string &app_name) {
            request->set_app_name(app_name);
            return *this;
        }
        SnsRequestBuilder&add_zone(const std::string &zone) {
            *request->mutable_zones()->Add() = zone;
            return *this;
        }
        SnsRequestBuilder&add_env(const std::string &env) {
            *request->mutable_env()->Add() = env;
            return *this;
        }
        SnsRequestBuilder&add_color(const std::string &color) {
            *request->mutable_color()->Add() = color;
            return *this;
        }

        std::shared_ptr<melon::SnsRequest> request;
    };

    class SnsNamingParams {
    public:
        turbo::Status register_service(const std::string &service_name, const std::shared_ptr<melon::SnsRequest> &req);

        turbo::Status update_service(const std::string &service_name, const std::shared_ptr<melon::SnsRequest> &req);

        std::shared_ptr<melon::SnsRequest> get_service(std::string_view service_name);

        static SnsNamingParams *get_instance() {
            static SnsNamingParams ins;
            return &ins;
        }

    private:
        turbo::Status check_service_name(turbo::Nonnull<const melon::SnsRequest *> req) const;

        SnsNamingParams() = default;

        std::shared_mutex _mutex;
        turbo::flat_hash_map<std::string, std::shared_ptr<melon::SnsRequest>> _services;

    };

    class SnsNamingService : public PeriodicNamingService {
    private:
        int GetServers(const char *service_name,
                       std::vector<ServerNode> *servers) override;

        void Describe(std::ostream &os, const DescribeOptions &) const override;

        NamingService *New() const override;

        void Destroy() override;

    private:
        SnsNamingClient _client;
    };
}  // namespace melon::naming

