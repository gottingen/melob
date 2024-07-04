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

#include <string>
#include <melon/utility/intrusive_ptr.hpp>               // mutil::intrusive_ptr
#include <melon/fiber/fiber.h>                    // fiber_t
#include <melon/rpc/server_id.h>                     // ServerId
#include <melon/rpc/shared_object.h>                 // SharedObject
#include <melon/naming/naming_service.h>                // NamingService
#include <melon/naming/naming_service_filter.h>         // NamingServiceFilter
#include <melon/rpc/socket_map.h>

namespace melon {

    // Inherit this class to observer NamingService changes.
    // NOTE: Same SocketId with different tags are treated as different entries.
    // When you change tag of a server, the server with the old tag will appear
    // in OnRemovedServers first, then in OnAddedServers with the new tag.
    class NamingServiceWatcher {
    public:
        virtual ~NamingServiceWatcher() {}

        virtual void OnAddedServers(const std::vector<ServerId> &servers) = 0;

        virtual void OnRemovedServers(const std::vector<ServerId> &servers) = 0;
    };

    struct GetNamingServiceThreadOptions {
        GetNamingServiceThreadOptions()
                : succeed_without_server(false), log_succeed_without_server(true), use_rdma(false) {}

        bool succeed_without_server;
        bool log_succeed_without_server;
        bool use_rdma;
        ChannelSignature channel_signature;
        std::shared_ptr<SocketSSLContext> ssl_ctx;
    };

    // A dedicated thread to map a name to ServerIds
    class NamingServiceThread : public SharedObject, public Describable {
        struct ServerNodeWithId {
            ServerNode node;
            SocketId id;

            inline bool operator<(const ServerNodeWithId &rhs) const {
                return id != rhs.id ? (id < rhs.id) : (node < rhs.node);
            }
        };

        class Actions : public NamingServiceActions {
        public:
            explicit Actions(NamingServiceThread *owner);

            ~Actions() override;

            void AddServers(const std::vector<ServerNode> &servers) override;

            void RemoveServers(const std::vector<ServerNode> &servers) override;

            void ResetServers(const std::vector<ServerNode> &servers) override;

            int WaitForFirstBatchOfServers();

            void EndWait(int error_code);

        private:
            NamingServiceThread *_owner;
            fiber_session_t _wait_id;
            std::atomic<bool> _has_wait_error;
            int _wait_error;
            std::vector<ServerNode> _last_servers;
            std::vector<ServerNode> _servers;
            std::vector<ServerNode> _added;
            std::vector<ServerNode> _removed;
            std::vector<ServerNodeWithId> _sockets;
            std::vector<ServerNodeWithId> _added_sockets;
            std::vector<ServerNodeWithId> _removed_sockets;
        };

    public:
        NamingServiceThread();

        ~NamingServiceThread() override;

        int Start(NamingService *ns,
                  const std::string &protocol,
                  const std::string &service_name,
                  const GetNamingServiceThreadOptions *options);

        int WaitForFirstBatchOfServers();

        void EndWait(int error_code);

        int AddWatcher(NamingServiceWatcher *w, const NamingServiceFilter *f);

        int AddWatcher(NamingServiceWatcher *w) { return AddWatcher(w, NULL); }

        int RemoveWatcher(NamingServiceWatcher *w);

        void Describe(std::ostream &os, const DescribeOptions &) const override;

    private:
        void Run();

        static void *RunThis(void *);

        static void ServerNodeWithId2ServerId(
                const std::vector<ServerNodeWithId> &src,
                std::vector<ServerId> *dst, const NamingServiceFilter *filter);

        mutil::Mutex _mutex;
        fiber_t _tid;
        NamingService *_ns;
        std::string _protocol;
        std::string _service_name;
        GetNamingServiceThreadOptions _options;
        std::vector<ServerNodeWithId> _last_sockets;
        Actions _actions;
        std::map<NamingServiceWatcher *, const NamingServiceFilter *> _watchers;
    };

    std::ostream &operator<<(std::ostream &os, const NamingServiceThread &);

    // Get the decicated thread associated with `url' and put the thread into
    // `ns_thread'. Calling with same `url' shares and returns the same thread.
    // If the url is not accessed before, this function blocks until the
    // NamingService returns the first batch of servers. If no servers are
    // available, unless `options->succeed_without_server' is on, this function
    // returns -1.
    // Returns 0 on success, -1 otherwise.
    int GetNamingServiceThread(mutil::intrusive_ptr<NamingServiceThread> *ns_thread,
                               const char *url,
                               const GetNamingServiceThreadOptions *options);

} // namespace melon

