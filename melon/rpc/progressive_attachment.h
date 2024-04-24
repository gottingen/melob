// Copyright 2023 The Elastic-AI Authors.
// part of Elastic AI Search
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//



#ifndef MELON_RPC_PROGRESSIVE_ATTACHMENT_H_
#define MELON_RPC_PROGRESSIVE_ATTACHMENT_H_

#include "melon/rpc/callback.h"
#include "melon/utility/atomicops.h"
#include "melon/utility/iobuf.h"
#include "melon/utility/endpoint.h"       // mutil::EndPoint
#include "melon/fiber/types.h"        // fiber_session_t
#include "melon/rpc/socket_id.h"       // SocketUniquePtr
#include "melon/rpc/shared_object.h"   // SharedObject

namespace melon {

class ProgressiveAttachment : public SharedObject {
friend class Controller;
public:
    // [Thread-safe]
    // Write `data' as one HTTP chunk to peer ASAP.
    // Returns 0 on success, -1 otherwise and errno is set.
    // Errnos are same as what Socket.Write may set.
    int Write(const mutil::IOBuf& data);
    int Write(const void* data, size_t n);

    // Get ip/port of peer/self.
    mutil::EndPoint remote_side() const;
    mutil::EndPoint local_side() const;

    // [Not thread-safe and can only be called once]
    // Run the callback when the underlying connection is broken (thus
    // transmission of the attachment is permanently stopped), or when
    // this attachment is destructed. In another word, the callback will
    // always be run.
    void NotifyOnStopped(google::protobuf::Closure* callback);
    
protected:
    // Transfer-Encoding is added since HTTP/1.1. If the protocol of the
    // response is before_http_1_1, we will write the data directly to the
    // socket without any futher modification and close the socket after all the
    // data has been written (so the client would receive EOF). Otherwise we
    // will encode each piece of data in the format of chunked-encoding.
    ProgressiveAttachment(SocketUniquePtr& movable_httpsock,
                          bool before_http_1_1);
    ~ProgressiveAttachment();

    // Called by controller only.
    void MarkRPCAsDone(bool rpc_failed);
    
    bool _before_http_1_1;
    bool _pause_from_mark_rpc_as_done;
    mutil::atomic<int> _rpc_state;
    mutil::Mutex _mutex;
    SocketUniquePtr _httpsock;
    mutil::IOBuf _saved_buf;
    fiber_session_t _notify_id;

private:
    static const int RPC_RUNNING;
    static const int RPC_SUCCEED;
    static const int RPC_FAILED;
};

} // namespace melon


#endif  // MELON_RPC_PROGRESSIVE_ATTACHMENT_H_
