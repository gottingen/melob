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


#ifndef  MELON_FIBER_EXECUTION_QUEUE_H_
#define  MELON_FIBER_EXECUTION_QUEUE_H_

#include <melon/fiber/fiber.h>
#include <melon/base/details/type_traits.h>  // mutil::add_const_reference

namespace fiber {

    // ExecutionQueue is a special wait-free MPSC queue of which the consumer thread
    // is auto started by the execute operation and auto quits if there are no more
    // tasks, in another word there isn't a daemon fiber waiting to consume tasks.

    template<typename T>
    struct ExecutionQueueId;

    template<typename T>
    class ExecutionQueue;

    struct TaskNode;

    class ExecutionQueueBase;

    class TaskIteratorBase {
        DISALLOW_COPY_AND_ASSIGN(TaskIteratorBase);

        friend class ExecutionQueueBase;

    public:
        // Returns true when the ExecutionQueue is stopped and there will never be
        // more tasks and you can safely release all the related resources ever
        // after.
        bool is_queue_stopped() const { return _is_stopped; }

        explicit operator bool() const;

    protected:
        TaskIteratorBase(TaskNode *head, ExecutionQueueBase *queue,
                         bool is_stopped, bool high_priority)
                : _cur_node(head), _head(head), _q(queue), _is_stopped(is_stopped), _high_priority(high_priority),
                  _should_break(false), _num_iterated(0) { operator++(); }

        ~TaskIteratorBase();

        void operator++();

        TaskNode *cur_node() const { return _cur_node; }

    private:
        int num_iterated() const { return _num_iterated; }

        bool should_break_for_high_priority_tasks();

        TaskNode *_cur_node;
        TaskNode *_head;
        ExecutionQueueBase *_q;
        bool _is_stopped;
        bool _high_priority;
        bool _should_break;
        int _num_iterated;
    };

    // Iterate over the given tasks
    //
    // Examples:
    // int demo_execute(void* meta, TaskIterator<T>& iter) {
    //     if (iter.is_queue_stopped()) {
    //         // destroy meta and related resources
    //         return 0;
    //     }
    //     for (; iter; ++iter) {
    //         // do_something(*iter)
    //         // or do_something(iter->a_member_of_T)
    //     }
    //     return 0;
    // }
    template<typename T>
    class TaskIterator : public TaskIteratorBase {
    public:
        typedef T *pointer;
        typedef T &reference;

        TaskIterator() = delete;

        reference operator*() const;

        pointer operator->() const { return &(operator*()); }

        TaskIterator &operator++();

        void operator++(int);
    };

    struct TaskHandle {
        TaskHandle();

        TaskNode *node;
        int64_t version;
    };

    struct TaskOptions {
        TaskOptions();

        TaskOptions(bool high_priority, bool in_place_if_possible);

        // Executor would execute high-priority tasks in the FIFO order but before
        // all pending normal-priority tasks.
        // NOTE: We don't guarantee any kind of real-time as there might be tasks still
        // in process which are uninterruptible.
        //
        // Default: false
        bool high_priority;

        // If |in_place_if_possible| is true, execution_queue_execute would call
        // execute immediately instead of starting a fiber if possible
        //
        // Note: Running callbacks in place might cause the deadlock issue, you
        // should be very careful turning this flag on.
        //
        // Default: false
        bool in_place_if_possible;
    };

    const static TaskOptions TASK_OPTIONS_NORMAL = TaskOptions(false, false);
    const static TaskOptions TASK_OPTIONS_URGENT = TaskOptions(true, false);
    const static TaskOptions TASK_OPTIONS_INPLACE = TaskOptions(false, true);

    class Executor {
    public:
        virtual ~Executor() = default;

        // Return 0 on success.
        virtual int submit(void *(*fn)(void *), void *args) = 0;
    };

    struct ExecutionQueueOptions {
        ExecutionQueueOptions();

        // Execute in resident pthread instead of fiber. default: false.
        bool use_pthread;

        // Attribute of the fiber which execute runs on. default: FIBER_ATTR_NORMAL
        // Fiber will be used when executor = NULL and use_pthread == false.
        fiber_attr_t fiber_attr;

        // Executor that tasks run on. default: NULL
        // Note that TaskOptions.in_place_if_possible = false will not work, if implementation of
        // Executor is in-place(synchronous).
        Executor *executor;
    };

    // Start an ExecutionQueue. If |options| is NULL, the queue will be created with
    // the default options.
    // Returns 0 on success, errno otherwise
    // NOTE: type |T| can be non-POD but must be copy-constructive
    template<typename T>
    int execution_queue_start(
            ExecutionQueueId<T> *id,
            const ExecutionQueueOptions *options,
            int (*execute)(void *meta, TaskIterator<T> &iter),
            void *meta);

    // Stop the ExecutionQueue.
    // After this function is called:
    //  - All the following calls to execution_queue_execute would fail immediately.
    //  - The executor will call |execute| with TaskIterator::is_queue_stopped() being
    //    true exactly once when all the pending tasks have been executed, and after
    //    this point it's ok to release the resource referenced by |meta|.
    // Returns 0 on success, errno otherwise.
    template<typename T>
    int execution_queue_stop(ExecutionQueueId<T> id);

    // Wait until the stop task (Iterator::is_queue_stopped() returns true) has
    // been executed
    template<typename T>
    int execution_queue_join(ExecutionQueueId<T> id);

    // Thread-safe and Wait-free.
    // Execute a task with default TaskOptions (normal task);
    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                typename mutil::add_const_reference<T>::type task);

    // Thread-safe and Wait-free.
    // Execute a task with options. e.g
    // fiber::execution_queue_execute(queue, task, &fiber::TASK_OPTIONS_URGENT)
    // If |options| is NULL, we will use default options (normal task)
    // If |handle| is not NULL, we will assign it with the handler of this task.
    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                typename mutil::add_const_reference<T>::type task,
                                const TaskOptions *options);

    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                typename mutil::add_const_reference<T>::type task,
                                const TaskOptions *options,
                                TaskHandle *handle);

    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                T &&task);

    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                T &&task,
                                const TaskOptions *options);

    template<typename T>
    int execution_queue_execute(ExecutionQueueId<T> id,
                                T &&task,
                                const TaskOptions *options,
                                TaskHandle *handle);

    // [Thread safe and ABA free] Cancel the corresponding task.
    // Returns:
    //  -1: The task was executed or h is an invalid handle
    //  0: Success
    //  1: The task is executing
    int execution_queue_cancel(const TaskHandle &h);

    // Thread-safe and Wait-free
    // Address a reference of ExecutionQueue if |id| references to a valid
    // ExecutionQueue
    //
    // |execution_queue_execute| internally fetches a reference of ExecutionQueue at
    // the beginning and releases it at the end, which makes 2 additional cache
    // updates. In some critical situation where the overhead of
    // execution_queue_execute matters, you can avoid this by addressing the
    // reference at the beginning of every producer, and execute tasks execatly
    // through the reference instead of id.
    //
    // Note: It makes |execution_queue_stop| a little complicated in the user level,
    // as we don't pass the `stop task' to |execute| until no one holds any reference.
    // If you are not sure about the ownership of the return value (which releases
    // the reference of the very ExecutionQueue in the destructor) and don't that
    // care the overhead of ExecutionQueue, DON'T use this function
    template<typename T>
    typename ExecutionQueue<T>::scoped_ptr_t
    execution_queue_address(ExecutionQueueId<T> id);

}  // namespace fiber

#include <melon/fiber/execution_queue_inl.h>

#endif  // MELON_FIBER_EXECUTION_QUEUE_H_
