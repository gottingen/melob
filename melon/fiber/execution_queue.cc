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


#include <melon/fiber/execution_queue.h>

#include <melon/utility/memory/singleton_on_pthread_once.h>
#include <melon/utility/object_pool.h>           // mutil::get_object
#include <melon/utility/resource_pool.h>         // mutil::get_resource
#include <melon/utility/threading/platform_thread.h>

namespace fiber {

    //May be false on different platforms
    //MELON_CASSERT(sizeof(TaskNode) == 128, sizeof_TaskNode_must_be_128);
    //MELON_CASSERT(offsetof(TaskNode, static_task_mem) + sizeof(TaskNode().static_task_mem) == 128, sizeof_TaskNode_must_be_128);
    MELON_CASSERT(sizeof(ExecutionQueue<int>) == sizeof(ExecutionQueueBase),
                  sizeof_ExecutionQueue_must_be_the_same_with_ExecutionQueueBase);
    MELON_CASSERT(sizeof(TaskIterator<int>) == sizeof(TaskIteratorBase),
                  sizeof_TaskIterator_must_be_the_same_with_TaskIteratorBase);
    namespace /*anonymous*/ {
        typedef mutil::ResourceId<ExecutionQueueBase> slot_id_t;

        inline slot_id_t WARN_UNUSED_RESULT slot_of_id(uint64_t id) {
            slot_id_t slot = {(id & 0xFFFFFFFFul)};
            return slot;
        }

        inline uint64_t make_id(uint32_t version, slot_id_t slot) {
            return (((uint64_t) version) << 32) | slot.value;
        }
    }  // namespace anonymous

    struct ExecutionQueueVars {
        melon::var::Adder<int64_t> running_task_count;
        melon::var::Adder<int64_t> execq_count;
        melon::var::Adder<int64_t> execq_active_count;

        ExecutionQueueVars();
    };

    ExecutionQueueVars::ExecutionQueueVars()
            : running_task_count("fiber_execq_running_task_count"), execq_count("fiber_execq_count"),
              execq_active_count("fiber_execq_active_count") {
    }

    inline ExecutionQueueVars *get_execq_vars() {
        return mutil::get_leaky_singleton<ExecutionQueueVars>();
    }

    void ExecutionQueueBase::start_execute(TaskNode *node) {
        node->next = TaskNode::UNCONNECTED;
        node->status = UNEXECUTED;
        node->iterated = false;
        if (node->high_priority) {
            // Add _high_priority_tasks before pushing this task into queue to
            // make sure that _execute_tasks sees the newest number when this
            // task is in the queue. Although there might be some useless for
            // loops in _execute_tasks if this thread is scheduled out at this
            // point, we think it's just fine.
            _high_priority_tasks.fetch_add(1, mutil::memory_order_relaxed);
        }
        TaskNode *const prev_head = _head.exchange(node, mutil::memory_order_release);
        if (prev_head != NULL) {
            node->next = prev_head;
            return;
        }
        // Get the right to execute the task, start a fiber to avoid deadlock
        // or stack overflow
        node->next = NULL;
        node->q = this;

        ExecutionQueueVars *const vars = get_execq_vars();
        vars->execq_active_count << 1;
        if (node->in_place) {
            int niterated = 0;
            _execute(node, node->high_priority, &niterated);
            TaskNode *tmp = node;
            // return if no more
            if (node->high_priority) {
                _high_priority_tasks.fetch_sub(niterated, mutil::memory_order_relaxed);
            }
            if (!_more_tasks(tmp, &tmp, !node->iterated)) {
                vars->execq_active_count << -1;
                return_task_node(node);
                return;
            }
        }

        if (nullptr == _options.executor) {
            if (_options.use_pthread) {
                if (_pthread_started) {
                    MELON_SCOPED_LOCK(_mutex);
                    _current_head = node;
                    _cond.Signal();
                } else {
                    // Start the execution fiber in background once.
                    if (pthread_create(&_pid, NULL,
                                       _execute_tasks_pthread,
                                       node) != 0) {
                        PMLOG(FATAL) << "Fail to create pthread";
                        _execute_tasks(node);
                    }
                    _pthread_started = true;
                }
            } else {
                fiber_t tid;
                // We start the execution fiber in background instead of foreground as
                // we can't determine whether the code after execute() is urgent (like
                // unlock a pthread_mutex_t) in which case implicit context switch may
                // cause undefined behavior (e.g. deadlock)
                if (fiber_start_background(&tid, &_options.fiber_attr,
                                           _execute_tasks, node) != 0) {
                    PMLOG(FATAL) << "Fail to start fiber";
                    _execute_tasks(node);
                }
            }

        } else {
            if (_options.executor->submit(_execute_tasks, node) != 0) {
                PMLOG(FATAL) << "Fail to submit task";
                _execute_tasks(node);
            }
        }
    }

    void *ExecutionQueueBase::_execute_tasks(void *arg) {
        ExecutionQueueVars *vars = get_execq_vars();
        TaskNode *head = (TaskNode *) arg;
        ExecutionQueueBase *m = (ExecutionQueueBase *) head->q;
        TaskNode *cur_tail = NULL;
        bool destroy_queue = false;
        for (;;) {
            if (head->iterated) {
                MCHECK(head->next != NULL);
                TaskNode *saved_head = head;
                head = head->next;
                m->return_task_node(saved_head);
            }
            int rc = 0;
            if (m->_high_priority_tasks.load(mutil::memory_order_relaxed) > 0) {
                int nexecuted = 0;
                // Don't care the return value
                rc = m->_execute(head, true, &nexecuted);
                m->_high_priority_tasks.fetch_sub(
                        nexecuted, mutil::memory_order_relaxed);
                if (nexecuted == 0) {
                    // Some high_priority tasks are not in queue
                    sched_yield();
                }
            } else {
                rc = m->_execute(head, false, NULL);
            }
            if (rc == ESTOP) {
                destroy_queue = true;
            }
            // Release TaskNode until uniterated task or last task
            while (head->next != NULL && head->iterated) {
                TaskNode *saved_head = head;
                head = head->next;
                m->return_task_node(saved_head);
            }
            if (cur_tail == NULL) {
                for (cur_tail = head; cur_tail->next != NULL;
                     cur_tail = cur_tail->next) {}
            }
            // break when no more tasks and head has been executed
            if (!m->_more_tasks(cur_tail, &cur_tail, !head->iterated)) {
                MCHECK_EQ(cur_tail, head);
                MCHECK(head->iterated);
                m->return_task_node(head);
                break;
            }
        }
        if (destroy_queue) {
            MCHECK(m->_head.load(mutil::memory_order_relaxed) == NULL);
            MCHECK(m->_stopped);
            // Add _join_butex by 2 to make it equal to the next version of the
            // ExecutionQueue from the same slot so that join with old id would
            // return immediately.
            //
            // 1: release fence to make join sees the newest changes when it sees
            //    the newest _join_butex
            m->_join_butex->fetch_add(2, mutil::memory_order_release/*1*/);
            butex_wake_all(m->_join_butex);
            vars->execq_count << -1;
            mutil::return_resource(slot_of_id(m->_this_id));
        }
        vars->execq_active_count << -1;
        return NULL;
    }

    void *ExecutionQueueBase::_execute_tasks_pthread(void *arg) {
        mutil::PlatformThread::SetName("ExecutionQueue");
        auto head = (TaskNode *) arg;
        auto m = (ExecutionQueueBase *) head->q;
        m->_current_head = head;
        while (true) {
            MELON_SCOPED_LOCK(m->_mutex);
            while (!m->_current_head) {
                m->_cond.Wait();
            }
            _execute_tasks(m->_current_head);
            m->_current_head = NULL;

            int expected = _version_of_id(m->_this_id);
            if (expected != m->_join_butex->load(mutil::memory_order_relaxed)) {
                // Execute queue has been stopped and stopped task has been executed, quit.
                break;
            }
        }
        return NULL;
    }

    void ExecutionQueueBase::return_task_node(TaskNode *node) {
        node->clear_before_return(_clear_func);
        mutil::return_object<TaskNode>(node);
        get_execq_vars()->running_task_count << -1;
    }

    void ExecutionQueueBase::_on_recycle() {
        // Push a closed tasks
        while (true) {
            TaskNode *node = mutil::get_object<TaskNode>();
            if (MELON_LIKELY(node != NULL)) {
                get_execq_vars()->running_task_count << 1;
                node->stop_task = true;
                node->high_priority = false;
                node->in_place = false;
                start_execute(node);
                break;
            }
            MCHECK(false) << "Fail to create task_node_t, " << berror();
            ::fiber_usleep(1000);
        }
    }

    int ExecutionQueueBase::join(uint64_t id) {
        const slot_id_t slot = slot_of_id(id);
        ExecutionQueueBase *const m = mutil::address_resource(slot);
        if (m == NULL) {
            // The queue is not created yet, this join is definitely wrong.
            return EINVAL;
        }
        int expected = _version_of_id(id);
        // acquire fence makes this thread see changes before changing _join_butex.
        while (expected == m->_join_butex->load(mutil::memory_order_acquire)) {
            if (butex_wait(m->_join_butex, expected, NULL) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return errno;
            }
        }
        // Join pthread if it's started.
        if (m->_options.use_pthread && m->_pthread_started) {
            pthread_join(m->_pid, NULL);
        }
        return 0;
    }

    int ExecutionQueueBase::stop() {
        const uint32_t id_ver = _version_of_id(_this_id);
        uint64_t vref = _versioned_ref.load(mutil::memory_order_relaxed);
        for (;;) {
            if (_version_of_vref(vref) != id_ver) {
                return EINVAL;
            }
            // Try to set version=id_ver+1 (to make later address() return NULL),
            // retry on fail.
            if (_versioned_ref.compare_exchange_strong(
                    vref, _make_vref(id_ver + 1, _ref_of_vref(vref)),
                    mutil::memory_order_release,
                    mutil::memory_order_relaxed)) {
                // Set _stopped to make lattern execute() fail immediately
                _stopped.store(true, mutil::memory_order_release);
                // Deref additionally which is added at creation so that this
                // queue's reference will hit 0(recycle) when no one addresses it.
                _release_additional_reference();
                // NOTE: This queue may be recycled at this point, don't
                // touch anything.
                return 0;
            }
        }
    }

    int ExecutionQueueBase::_execute(TaskNode *head, bool high_priority, int *niterated) {
        if (head != NULL && head->stop_task) {
            MCHECK(head->next == NULL);
            head->iterated = true;
            head->status = EXECUTED;
            TaskIteratorBase iter(NULL, this, true, false);
            _execute_func(_meta, _type_specific_function, iter);
            if (niterated) {
                *niterated = 1;
            }
            return ESTOP;
        }
        TaskIteratorBase iter(head, this, false, high_priority);
        if (iter) {
            _execute_func(_meta, _type_specific_function, iter);
        }
        // We must assign |niterated| with num_iterated even if we couldn't peek
        // any task to execute at the begining, in which case all the iterated
        // tasks have been cancelled at this point. And we must return the
        // correct num_iterated() to the caller to update the counter correctly.
        if (niterated) {
            *niterated = iter.num_iterated();
        }
        return 0;
    }

    TaskNode *ExecutionQueueBase::allocate_node() {
        get_execq_vars()->running_task_count << 1;
        return mutil::get_object<TaskNode>();
    }

    TaskNode *const TaskNode::UNCONNECTED = (TaskNode *) -1L;

    ExecutionQueueBase::scoped_ptr_t ExecutionQueueBase::address(uint64_t id) {
        scoped_ptr_t ret;
        const slot_id_t slot = slot_of_id(id);
        ExecutionQueueBase *const m = mutil::address_resource(slot);
        if (MELON_LIKELY(m != NULL)) {
            // acquire fence makes sure this thread sees latest changes before
            // _dereference()
            const uint64_t vref1 = m->_versioned_ref.fetch_add(
                    1, mutil::memory_order_acquire);
            const uint32_t ver1 = _version_of_vref(vref1);
            if (ver1 == _version_of_id(id)) {
                ret.reset(m);
                return ret.Pass();
            }

            const uint64_t vref2 = m->_versioned_ref.fetch_sub(
                    1, mutil::memory_order_release);
            const int32_t nref = _ref_of_vref(vref2);
            if (nref > 1) {
                return ret.Pass();
            } else if (__builtin_expect(nref == 1, 1)) {
                const uint32_t ver2 = _version_of_vref(vref2);
                if ((ver2 & 1)) {
                    if (ver1 == ver2 || ver1 + 1 == ver2) {
                        uint64_t expected_vref = vref2 - 1;
                        if (m->_versioned_ref.compare_exchange_strong(
                                expected_vref, _make_vref(ver2 + 1, 0),
                                mutil::memory_order_acquire,
                                mutil::memory_order_relaxed)) {
                            m->_on_recycle();
                            // We don't return m immediatly when the reference count
                            // reaches 0 as there might be in processing tasks. Instead
                            // _on_recycle would push a `stop_task', after which
                            // is executed m would be finally reset and returned
                        }
                    } else {
                        MCHECK(false) << "ref-version=" << ver1
                                      << " unref-version=" << ver2;
                    }
                } else {
                    MCHECK_EQ(ver1, ver2);
                    // Addressed a free slot.
                }
            } else {
                MCHECK(false) << "Over dereferenced id=" << id;
            }
        }
        return ret.Pass();
    }

    int ExecutionQueueBase::create(uint64_t *id, const ExecutionQueueOptions *options,
                                   execute_func_t execute_func,
                                   clear_task_mem clear_func,
                                   void *meta, void *type_specific_function) {
        if (execute_func == NULL || clear_func == NULL) {
            return EINVAL;
        }

        slot_id_t slot;
        ExecutionQueueBase *const m = mutil::get_resource(&slot, Forbidden());
        if (MELON_LIKELY(m != NULL)) {
            m->_execute_func = execute_func;
            m->_clear_func = clear_func;
            m->_meta = meta;
            m->_type_specific_function = type_specific_function;
            MCHECK(m->_head.load(mutil::memory_order_relaxed) == NULL);
            MCHECK_EQ(0, m->_high_priority_tasks.load(mutil::memory_order_relaxed));
            ExecutionQueueOptions opt;
            if (options != NULL) {
                opt = *options;
            }
            m->_options = opt;
            m->_stopped.store(false, mutil::memory_order_relaxed);
            m->_this_id = make_id(
                    _version_of_vref(m->_versioned_ref.fetch_add(
                            1, mutil::memory_order_release)), slot);
            *id = m->_this_id;
            m->_pthread_started = false;
            m->_current_head = NULL;
            get_execq_vars()->execq_count << 1;
            return 0;
        }
        return ENOMEM;
    }

    inline bool TaskIteratorBase::should_break_for_high_priority_tasks() {
        if (!_high_priority &&
            _q->_high_priority_tasks.load(mutil::memory_order_relaxed) > 0) {
            _should_break = true;
            return true;
        }
        return false;
    }

    void TaskIteratorBase::operator++() {
        if (!(*this)) {
            return;
        }
        if (_cur_node->iterated) {
            _cur_node = _cur_node->next;
        }
        if (should_break_for_high_priority_tasks()) {
            return;
        }  // else the next high_priority_task would be delayed for at most one task

        while (_cur_node && !_cur_node->stop_task) {
            if (_high_priority == _cur_node->high_priority) {
                if (!_cur_node->iterated && _cur_node->peek_to_execute()) {
                    ++_num_iterated;
                    _cur_node->iterated = true;
                    return;
                }
                _num_iterated += !_cur_node->iterated;
                _cur_node->iterated = true;
            }
            _cur_node = _cur_node->next;
        }
        return;
    }

    TaskIteratorBase::~TaskIteratorBase() {
        // Set the iterated tasks as EXECUTED here instead of waiting them to be
        // returned in _start_execute as the high_priority_task might be in the
        // middle of the linked list and is not going to be returned soon
        if (_is_stopped) {
            return;
        }
        while (_head != _cur_node) {
            if (_head->iterated && _head->high_priority == _high_priority) {
                _head->set_executed();
            }
            _head = _head->next;
        }
        if (_should_break && _cur_node != NULL
            && _cur_node->high_priority == _high_priority && _cur_node->iterated) {
            _cur_node->set_executed();
        }
    }

} // namespace fiber
