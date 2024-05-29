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

#include <pthread.h>                        // pthread_mutex_*
#include <cstdlib>                         // abort

#include <new>                              // std::nothrow
#include <deque>                            // std::deque
#include <vector>                           // std::vector

#include <melon/utility/errno.h>                     // errno
#include <melon/utility/thread_local.h>              // thread_atexit
#include <melon/utility/macros.h>                    // MELON_CACHELINE_ALIGNMENT
#include <melon/utility/scoped_lock.h>
#include <turbo/log/logging.h>

namespace melon::var {
    namespace detail {

        typedef int AgentId;

// General NOTES:
// * Don't use bound-checking vector::at.
// * static functions in template class are not guaranteed to be inlined,
//   add inline keyword explicitly.
// * Put fast path in "if" branch, which is more cpu-wise.
// * don't use __builtin_expect excessively because CPU may predict the branch
//   better than you. Only hint branches that are definitely unusual.

        template<typename Agent>
        class AgentGroup {
        public:
            typedef Agent agent_type;

            // TODO: We should remove the template parameter and unify AgentGroup
            // of all var with a same one, to reuse the memory between different
            // type of var. The unified AgentGroup allocates small structs in-place
            // and large structs on heap, thus keeping batch efficiencies on small
            // structs and improving memory usage on large structs.
            const static size_t RAW_BLOCK_SIZE = 4096;
            const static size_t ELEMENTS_PER_BLOCK =
                    (RAW_BLOCK_SIZE + sizeof(Agent) - 1) / sizeof(Agent);

            // The most generic method to allocate agents is to call ctor when
            // agent is needed, however we construct all agents when initializing
            // ThreadBlock, which has side effects:
            //  * calling ctor ELEMENTS_PER_BLOCK times is slower.
            //  * calling ctor of non-pod types may be unpredictably slow.
            //  * non-pod types may allocate space inside ctor excessively.
            //  * may return non-null for unexist id.
            //  * lifetime of agent is more complex. User has to reset the agent before
            //    destroying id otherwise when the agent is (implicitly) reused by
            //    another one who gets the reused id, things are screwed.
            // TODO(chenzhangyi01): To fix these problems, a method is to keep a bitmap
            // along with ThreadBlock* in _s_tls_blocks, each bit in the bitmap
            // represents that the agent is constructed or not. Drawback of this method
            // is that the bitmap may take 32bytes (for 256 agents, which is common) so
            // that addressing on _s_tls_blocks may be slower if identifiers distribute
            // sparsely. Another method is to put the bitmap in ThreadBlock. But this
            // makes alignment of ThreadBlock harder and to address the agent we have
            // to touch an additional cacheline: the bitmap. Whereas in the first
            // method, bitmap and ThreadBlock* are in one cacheline.
            struct MELON_CACHELINE_ALIGNMENT ThreadBlock {
                inline Agent *at(size_t offset) { return _agents + offset; };

            private:
                Agent _agents[ELEMENTS_PER_BLOCK];
            };

            inline static AgentId create_new_agent() {
                MELON_SCOPED_LOCK(_s_mutex);
                AgentId agent_id = 0;
                if (!_get_free_ids().empty()) {
                    agent_id = _get_free_ids().back();
                    _get_free_ids().pop_back();
                } else {
                    agent_id = _s_agent_kinds++;
                }
                return agent_id;
            }

            inline static int destroy_agent(AgentId id) {
                // TODO: How to avoid double free?
                MELON_SCOPED_LOCK(_s_mutex);
                if (id < 0 || id >= _s_agent_kinds) {
                    errno = EINVAL;
                    return -1;
                }
                _get_free_ids().push_back(id);
                return 0;
            }

            // Note: May return non-null for unexist id, see notes on ThreadBlock
            // We need this function to be as fast as possible.
            inline static Agent *get_tls_agent(AgentId id) {
                if (__builtin_expect(id >= 0, 1)) {
                    if (_s_tls_blocks) {
                        const size_t block_id = (size_t) id / ELEMENTS_PER_BLOCK;
                        if (block_id < _s_tls_blocks->size()) {
                            ThreadBlock *const tb = (*_s_tls_blocks)[block_id];
                            if (tb) {
                                return tb->at(id - block_id * ELEMENTS_PER_BLOCK);
                            }
                        }
                    }
                }
                return NULL;
            }

            // Note: May return non-null for unexist id, see notes on ThreadBlock
            inline static Agent *get_or_create_tls_agent(AgentId id) {
                if (__builtin_expect(id < 0, 0)) {
                    CHECK(false) << "Invalid id=" << id;
                    return NULL;
                }
                if (_s_tls_blocks == NULL) {
                    _s_tls_blocks = new(std::nothrow) std::vector<ThreadBlock *>;
                    if (__builtin_expect(_s_tls_blocks == NULL, 0)) {
                        LOG(FATAL) << "Fail to create vector, " << berror();
                        return NULL;
                    }
                    mutil::thread_atexit(_destroy_tls_blocks);
                }
                const size_t block_id = (size_t) id / ELEMENTS_PER_BLOCK;
                if (block_id >= _s_tls_blocks->size()) {
                    // The 32ul avoid pointless small resizes.
                    _s_tls_blocks->resize(std::max(block_id + 1, 32ul));
                }
                ThreadBlock *tb = (*_s_tls_blocks)[block_id];
                if (tb == NULL) {
                    ThreadBlock *new_block = new(std::nothrow) ThreadBlock;
                    if (__builtin_expect(new_block == NULL, 0)) {
                        return NULL;
                    }
                    tb = new_block;
                    (*_s_tls_blocks)[block_id] = new_block;
                }
                return tb->at(id - block_id * ELEMENTS_PER_BLOCK);
            }

        private:
            static void _destroy_tls_blocks() {
                if (!_s_tls_blocks) {
                    return;
                }
                for (size_t i = 0; i < _s_tls_blocks->size(); ++i) {
                    delete (*_s_tls_blocks)[i];
                }
                delete _s_tls_blocks;
                _s_tls_blocks = NULL;
            }

            inline static std::deque<AgentId> &_get_free_ids() {
                if (__builtin_expect(!_s_free_ids, 0)) {
                    _s_free_ids = new(std::nothrow) std::deque<AgentId>();
                    RELEASE_ASSERT(_s_free_ids);
                }
                return *_s_free_ids;
            }

            static pthread_mutex_t _s_mutex;
            static AgentId _s_agent_kinds;
            static std::deque<AgentId> *_s_free_ids;
            static __thread std::vector<ThreadBlock *> *_s_tls_blocks;
        };

        template<typename Agent>
        pthread_mutex_t AgentGroup<Agent>::_s_mutex = PTHREAD_MUTEX_INITIALIZER;

        template<typename Agent>
        std::deque<AgentId> *AgentGroup<Agent>::_s_free_ids = NULL;

        template<typename Agent>
        AgentId AgentGroup<Agent>::_s_agent_kinds = 0;

        template<typename Agent>
        __thread std::vector<typename AgentGroup<Agent>::ThreadBlock *>
                *AgentGroup<Agent>::_s_tls_blocks = NULL;

    }  // namespace detail
}  // namespace melon::var
