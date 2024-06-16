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

#include <string>                       // std::string
#include <vector>                       // std::vector
#include <atomic>
#include <melon/base/scoped_lock.h>           // MELON_SCOPED_LOCK
#include <type_traits>
#include <mutex>
#include <melon/base/linked_list.h>// LinkNode
#include <melon/var/detail/agent_group.h>    // detail::AgentGroup
#include <melon/base/details/type_traits.h>
#include <melon/var/detail/call_op_returning_void.h>

namespace melon::var {
    namespace detail {

// Parameter to merge_global.
        template<typename Combiner>
        class GlobalValue {
        public:
            typedef typename Combiner::result_type result_type;
            typedef typename Combiner::Agent agent_type;

            GlobalValue(agent_type *a, Combiner *c) : _a(a), _c(c) {}

            ~GlobalValue() {}

            // Call this method to unlock tls element and lock the combiner.
            // Unlocking tls element avoids potential deadlock with
            // AgentCombiner::reset(), which also means that tls element may be
            // changed during calling of this method. BE AWARE OF THIS!
            // After this method is called (and before unlock), tls element and
            // global_result will not be changed provided this method is called
            // from the thread owning the agent.
            result_type *lock() {
                _a->element._lock.unlock();
                _c->_lock.lock();
                return &_c->_global_result;
            }

            // Call this method to unlock the combiner and lock tls element again.
            void unlock() {
                _c->_lock.unlock();
                _a->element._lock.lock();
            }

        private:
            agent_type *_a;
            Combiner *_c;
        };

        // Abstraction of tls element whose operations are all atomic.
        template<typename T, typename Enabler = void>
        class ElementContainer {
            template<typename> friend
            class GlobalValue;

        public:
            void load(T *out) {
                std::unique_lock guard(_lock);
                *out = _value;
            }

            void store(const T &new_value) {
                std::unique_lock guard(_lock);
                _value = new_value;
            }

            void exchange(T *prev, const T &new_value) {
                std::unique_lock guard(_lock);
                *prev = _value;
                _value = new_value;
            }

            template<typename Op, typename T1>
            void modify(const Op &op, const T1 &value2) {
                std::unique_lock guard(_lock);
                call_op_returning_void(op, _value, value2);
            }

            // [Unique]
            template<typename Op, typename GlobalValue>
            void merge_global(const Op &op, GlobalValue &global_value) {
                std::unique_lock guard(_lock);
                op(global_value, _value);
            }

        private:
            T _value;
            std::mutex _lock;
        };

        template<typename T>
        class ElementContainer<
                T, typename std::enable_if<mutil::is_atomical<T>::value>::type> {
        public:
            // We don't need any memory fencing here, every op is relaxed.

            inline void load(T *out) {
                *out = _value.load(std::memory_order_relaxed);
            }

            inline void store(T new_value) {
                _value.store(new_value, std::memory_order_relaxed);
            }

            inline void exchange(T *prev, T new_value) {
                *prev = _value.exchange(new_value, std::memory_order_relaxed);
            }

            // [Unique]
            inline bool compare_exchange_weak(T &expected, T new_value) {
                return _value.compare_exchange_weak(expected, new_value,
                                                    std::memory_order_relaxed);
            }

            template<typename Op, typename T1>
            void modify(const Op &op, const T1 &value2) {
                T old_value = _value.load(std::memory_order_relaxed);
                T new_value = old_value;
                call_op_returning_void(op, new_value, value2);
                // There's a contention with the reset operation of combiner,
                // if the tls value has been modified during _op, the
                // compare_exchange_weak operation will fail and recalculation is
                // to be processed according to the new version of value
                while (!_value.compare_exchange_weak(
                        old_value, new_value, std::memory_order_relaxed)) {
                    new_value = old_value;
                    call_op_returning_void(op, new_value, value2);
                }
            }

        private:
            std::atomic<T> _value;
        };

        template<typename ResultTp, typename ElementTp, typename BinaryOp>
        class AgentCombiner {
        public:
            typedef ResultTp result_type;
            typedef ElementTp element_type;
            typedef AgentCombiner<ResultTp, ElementTp, BinaryOp> self_type;

            friend class GlobalValue<self_type>;

            struct Agent : public mutil::LinkNode<Agent> {
                Agent() : combiner(NULL) {}

                ~Agent() {
                    if (combiner) {
                        combiner->commit_and_erase(this);
                        combiner = NULL;
                    }
                }

                void reset(const ElementTp &val, self_type *c) {
                    combiner = c;
                    element.store(val);
                }

                // Call op(GlobalValue<Combiner> &, ElementTp &) to merge tls element
                // into global_result. The common impl. is:
                //   struct XXX {
                //       void operator()(GlobalValue<Combiner> & global_value,
                //                       ElementTp & local_value) const {
                //           if (test_for_merging(local_value)) {
                //
                //               // Unlock tls element and lock combiner. Obviously
                //               // tls element can be changed during lock().
                //               ResultTp* g = global_value.lock();
                //
                //               // *g and local_value are not changed provided
                //               // merge_global is called from the thread owning
                //               // the agent.
                //               merge(*g, local_value);
                //
                //               // unlock combiner and lock tls element again.
                //               global_value.unlock();
                //           }
                //
                //           // safe to modify local_value because it's already locked
                //           // or locked again after merging.
                //           ...
                //       }
                //   };
                //
                // NOTE: Only available to non-atomic types.
                template<typename Op>
                void merge_global(const Op &op) {
                    GlobalValue<self_type> g(this, combiner);
                    element.merge_global(op, g);
                }

                self_type *combiner;
                ElementContainer<ElementTp> element;
            };

            typedef detail::AgentGroup<Agent> AgentGroup;

            explicit AgentCombiner(const ResultTp result_identity = ResultTp(),
                                   const ElementTp element_identity = ElementTp(),
                                   const BinaryOp &op = BinaryOp())
                    : _id(AgentGroup::create_new_agent()), _op(op), _global_result(result_identity),
                      _result_identity(result_identity), _element_identity(element_identity) {
            }

            ~AgentCombiner() {
                if (_id >= 0) {
                    clear_all_agents();
                    AgentGroup::destroy_agent(_id);
                    _id = -1;
                }
            }

            // [Threadsafe] May be called from anywhere
            ResultTp combine_agents() const {
                ElementTp tls_value;
                std::unique_lock guard(_lock);
                ResultTp ret = _global_result;
                for (mutil::LinkNode<Agent> *node = _agents.head();
                     node != _agents.end(); node = node->next()) {
                    node->value()->element.load(&tls_value);
                    call_op_returning_void(_op, ret, tls_value);
                }
                return ret;
            }

            typename mutil::add_cr_non_integral<ElementTp>::type element_identity() const { return _element_identity; }

            typename  mutil::add_cr_non_integral<ResultTp>::type result_identity() const { return _result_identity; }

            // [Threadsafe] May be called from anywhere.
            ResultTp reset_all_agents() {
                ElementTp prev;
                std::unique_lock guard(_lock);
                ResultTp tmp = _global_result;
                _global_result = _result_identity;
                for (mutil::LinkNode<Agent> *node = _agents.head();
                     node != _agents.end(); node = node->next()) {
                    node->value()->element.exchange(&prev, _element_identity);
                    call_op_returning_void(_op, tmp, prev);
                }
                return tmp;
            }

            // Always called from the thread owning the agent.
            void commit_and_erase(Agent *agent) {
                if (NULL == agent) {
                    return;
                }
                ElementTp local;
                std::unique_lock guard(_lock);
                // TODO: For non-atomic types, we can pass the reference to op directly.
                // But atomic types cannot. The code is a little troublesome to write.
                agent->element.load(&local);
                call_op_returning_void(_op, _global_result, local);
                agent->RemoveFromList();
            }

            // Always called from the thread owning the agent
            void commit_and_clear(Agent *agent) {
                if (NULL == agent) {
                    return;
                }
                ElementTp prev;
                std::unique_lock guard(_lock);
                agent->element.exchange(&prev, _element_identity);
                call_op_returning_void(_op, _global_result, prev);
            }

            // We need this function to be as fast as possible.
            inline Agent *get_or_create_tls_agent() {
                Agent *agent = AgentGroup::get_tls_agent(_id);
                if (!agent) {
                    // Create the agent
                    agent = AgentGroup::get_or_create_tls_agent(_id);
                    if (NULL == agent) {
                        LOG(FATAL) << "Fail to create agent";
                        return NULL;
                    }
                }
                if (agent->combiner) {
                    return agent;
                }
                agent->reset(_element_identity, this);
                // TODO: Is uniqueness-checking necessary here?
                {
                    std::unique_lock guard(_lock);
                    _agents.Append(agent);
                }
                return agent;
            }

            void clear_all_agents() {
                std::unique_lock guard(_lock);
                // reseting agents is must because the agent object may be reused.
                // Set element to be default-constructed so that if it's non-pod,
                // internal allocations should be released.
                for (mutil::LinkNode<Agent> *
                        node = _agents.head(); node != _agents.end();) {
                    node->value()->reset(ElementTp(), NULL);
                    mutil::LinkNode<Agent> *const saved_next = node->next();
                    node->RemoveFromList();
                    node = saved_next;
                }
            }

            const BinaryOp &op() const { return _op; }

            bool valid() const { return _id >= 0; }

        private:
            AgentId _id;
            BinaryOp _op;
            mutable std::mutex _lock;
            ResultTp _global_result;
            ResultTp _result_identity;
            ElementTp _element_identity;
            mutil::LinkedList<Agent> _agents;
        };

    }  // namespace detail
}  // namespace melon::var
