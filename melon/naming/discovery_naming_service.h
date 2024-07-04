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



#ifndef MELON_NAMING_DISCOVERY_NAMING_SERVICE_H_
#define MELON_NAMING_DISCOVERY_NAMING_SERVICE_H_

#include <melon/naming/periodic_naming_service.h>
#include <melon/rpc/channel.h>
#include <melon/utility/synchronization/lock.h>

namespace melon::naming {

    struct DiscoveryRegisterParam {
        std::string appid;
        std::string hostname;
        std::string env;
        std::string zone;
        std::string region;
        std::string addrs;          // splitted by ','
        int status;
        std::string version;
        std::string metadata;

        bool IsValid() const;
    };

    // ONE DiscoveryClient corresponds to ONE service instance.
    // If your program has multiple service instances to register,
    // you need multiple DiscoveryClient.
    // Note: Cancel to the server is automatically called in dtor.
    class DiscoveryClient {
    public:
        DiscoveryClient();

        ~DiscoveryClient();

        // Initialize this client.
        // Returns 0 on success.
        // NOTE: Calling more than once does nothing and returns 0.
        int Register(const DiscoveryRegisterParam &req);

    private:
        static void *PeriodicRenew(void *arg);

        int DoCancel() const;

        int DoRegister();

        int DoRenew() const;

    private:
        fiber_t _th;
        std::atomic<bool> _registered;
        DiscoveryRegisterParam _params;
        mutil::EndPoint _current_discovery_server;
    };

    class DiscoveryNamingService : public PeriodicNamingService {
    private:
        int GetServers(const char *service_name,
                       std::vector<ServerNode> *servers) override;

        void Describe(std::ostream &os, const DescribeOptions &) const override;

        NamingService *New() const override;

        void Destroy() override;

    private:
        DiscoveryClient _client;
    };


} // namespace melon::naming

#endif // MELON_NAMING_DISCOVERY_NAMING_SERVICE_H_
