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


#include <sys/types.h>
#include <stddef.h>                         // size_t
#include <melon/utility/compat.h>                   // OS_MACOSX
#include <melon/utility/macros.h>                   // ARRAY_SIZE
#include <melon/utility/scoped_lock.h>              // MELON_SCOPED_LOCK
#include <melon/utility/fast_rand.h>
#include <melon/utility/unique_ptr.h>
#include <melon/utility/third_party/murmurhash3/murmurhash3.h> // fmix64
#include <melon/fiber/errno.h>                  // ESTOP
#include <melon/fiber/butex.h>                  // butex_*
#include <melon/fiber/sys_futex.h>              // futex_wake_private
#include <melon/fiber/processor.h>              // cpu_relax
#include <melon/fiber/task_control.h>
#include <melon/fiber/task_group.h>
#include <melon/fiber/timer_thread.h>
#include <melon/fiber/errno.h>
#include <turbo/flags/flag.h>

TURBO_FLAG(bool, show_fiber_creation_in_vars, false, "When this flags is on, The time "
                                                     "from fiber creation to first run will be recorded and shown "
                                                     "in /vars").on_validate(turbo::AllPassValidator<bool>::validate);
TURBO_FLAG(bool, show_per_worker_usage_in_vars, false,
           "Show per-worker usage in /vars/fiber_per_worker_usage_<tid>").on_validate(
        turbo::AllPassValidator<bool>::validate);
namespace fiber {

    static const fiber_attr_t FIBER_ATTR_TASKGROUP = {
            FIBER_STACKTYPE_UNKNOWN, 0, NULL, FIBER_TAG_INVALID};

    MELON_VOLATILE_THREAD_LOCAL(TaskGroup*, tls_task_group, NULL);
    // Sync with TaskMeta::local_storage when a fiber is created or destroyed.
    // During running, the two fields may be inconsistent, use tls_bls as the
    // groundtruth.
    __thread LocalStorage tls_bls = FIBER_LOCAL_STORAGE_INITIALIZER;

    // defined in fiber/key.cpp
    extern void return_keytable(fiber_keytable_pool_t *, KeyTable *);

    // [Hacky] This is a special TLS set by fiber-rpc privately... to save
    // overhead of creation keytable, may be removed later.
    MELON_VOLATILE_THREAD_LOCAL(void*, tls_unique_user_ptr, NULL);

    const TaskStatistics EMPTY_STAT = {0, 0};

    const size_t OFFSET_TABLE[] = {
#include "melon/fiber/offset_inl.list"
    };

    int TaskGroup::get_attr(fiber_t tid, fiber_attr_t *out) {
        TaskMeta *const m = address_meta(tid);
        if (m != NULL) {
            const uint32_t given_ver = get_version(tid);
            MELON_SCOPED_LOCK(m->version_lock);
            if (given_ver == *m->version_butex) {
                *out = m->attr;
                return 0;
            }
        }
        errno = EINVAL;
        return -1;
    }

    void TaskGroup::set_stopped(fiber_t tid) {
        TaskMeta *const m = address_meta(tid);
        if (m != NULL) {
            const uint32_t given_ver = get_version(tid);
            MELON_SCOPED_LOCK(m->version_lock);
            if (given_ver == *m->version_butex) {
                m->stop = true;
            }
        }
    }

    bool TaskGroup::is_stopped(fiber_t tid) {
        TaskMeta *const m = address_meta(tid);
        if (m != NULL) {
            const uint32_t given_ver = get_version(tid);
            MELON_SCOPED_LOCK(m->version_lock);
            if (given_ver == *m->version_butex) {
                return m->stop;
            }
        }
        // If the tid does not exist or version does not match, it's intuitive
        // to treat the thread as "stopped".
        return true;
    }

    bool TaskGroup::wait_task(fiber_t *tid) {
        do {
#ifndef FIBER_DONT_SAVE_PARKING_STATE
            if (_last_pl_state.stopped()) {
                return false;
            }
            _pl->wait(_last_pl_state);
            if (steal_task(tid)) {
                return true;
            }
#else
            const ParkingLot::State st = _pl->get_state();
            if (st.stopped()) {
                return false;
            }
            if (steal_task(tid)) {
                return true;
            }
            _pl->wait(st);
#endif
        } while (true);
    }

    static double get_cumulated_cputime_from_this(void *arg) {
        return static_cast<TaskGroup *>(arg)->cumulated_cputime_ns() / 1000000000.0;
    }

    void TaskGroup::run_main_task() {
        melon::var::PassiveStatus<double> cumulated_cputime(
                get_cumulated_cputime_from_this, this);
        std::unique_ptr<melon::var::PerSecond<melon::var::PassiveStatus<double> > > usage_var;

        TaskGroup *dummy = this;
        fiber_t tid;
        while (wait_task(&tid)) {
            TaskGroup::sched_to(&dummy, tid);
            DCHECK_EQ(this, dummy);
            DCHECK_EQ(_cur_meta->stack, _main_stack);
            if (_cur_meta->tid != _main_tid) {
                TaskGroup::task_runner(1/*skip remained*/);
            }
            if (turbo::get_flag(FLAGS_show_per_worker_usage_in_vars) && !usage_var) {
                char name[32];
#if defined(OS_MACOSX)
                snprintf(name, sizeof(name), "fiber_worker_usage_%" PRIu64,
                         pthread_numeric_id());
#else
                snprintf(name, sizeof(name), "fiber_worker_usage_%ld",
                         (long) syscall(SYS_gettid));
#endif
                usage_var.reset(new melon::var::PerSecond<melon::var::PassiveStatus<double> >
                                        (name, &cumulated_cputime, 1));
            }
        }
        // Don't forget to add elapse of last wait_task.
        current_task()->stat.cputime_ns += mutil::cpuwide_time_ns() - _last_run_ns;
    }

    TaskGroup::TaskGroup(TaskControl *c)
            :
            _cur_meta(NULL), _control(c), _num_nosignal(0), _nsignaled(0), _last_run_ns(mutil::cpuwide_time_ns()),
            _cumulated_cputime_ns(0), _nswitch(0), _last_context_remained(NULL), _last_context_remained_arg(NULL),
            _pl(NULL), _main_stack(NULL), _main_tid(0), _remote_num_nosignal(0), _remote_nsignaled(0)
#ifndef NDEBUG
            , _sched_recursive_guard(0)
#endif
            , _tag(FIBER_TAG_DEFAULT) {
        _steal_seed = mutil::fast_rand();
        _steal_offset = OFFSET_TABLE[_steal_seed % ARRAY_SIZE(OFFSET_TABLE)];
        CHECK(c);
    }

    TaskGroup::~TaskGroup() {
        if (_main_tid) {
            TaskMeta *m = address_meta(_main_tid);
            CHECK(_main_stack == m->stack);
            return_stack(m->release_stack());
            return_resource(get_slot(_main_tid));
            _main_tid = 0;
        }
    }

    int TaskGroup::init(size_t runqueue_capacity) {
        if (_rq.init(runqueue_capacity) != 0) {
            LOG(FATAL) << "Fail to init _rq";
            return -1;
        }
        if (_remote_rq.init(runqueue_capacity / 2) != 0) {
            LOG(FATAL) << "Fail to init _remote_rq";
            return -1;
        }
        ContextualStack *stk = get_stack(STACK_TYPE_MAIN, NULL);
        if (NULL == stk) {
            LOG(FATAL) << "Fail to get main stack container";
            return -1;
        }
        mutil::ResourceId<TaskMeta> slot;
        TaskMeta *m = mutil::get_resource<TaskMeta>(&slot);
        if (NULL == m) {
            LOG(FATAL) << "Fail to get TaskMeta";
            return -1;
        }
        m->stop = false;
        m->interrupted = false;
        m->about_to_quit = false;
        m->fn = NULL;
        m->arg = NULL;
        m->local_storage = LOCAL_STORAGE_INIT;
        m->cpuwide_start_ns = mutil::cpuwide_time_ns();
        m->stat = EMPTY_STAT;
        m->attr = FIBER_ATTR_TASKGROUP;
        m->tid = make_tid(*m->version_butex, slot);
        m->set_stack(stk);

        _cur_meta = m;
        _main_tid = m->tid;
        _main_stack = stk;
        _last_run_ns = mutil::cpuwide_time_ns();
        return 0;
    }

    void TaskGroup::task_runner(intptr_t skip_remained) {
        // NOTE: tls_task_group is volatile since tasks are moved around
        //       different groups.
        TaskGroup *g = tls_task_group;

        if (!skip_remained) {
            while (g->_last_context_remained) {
                RemainedFn fn = g->_last_context_remained;
                g->_last_context_remained = NULL;
                fn(g->_last_context_remained_arg);
                g = MELON_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
            }

#ifndef NDEBUG
            --g->_sched_recursive_guard;
#endif
        }

        do {
            // A task can be stopped before it gets running, in which case
            // we may skip user function, but that may confuse user:
            // Most tasks have variables to remember running result of the task,
            // which is often initialized to values indicating success. If an
            // user function is never called, the variables will be unchanged
            // however they'd better reflect failures because the task is stopped
            // abnormally.

            // Meta and identifier of the task is persistent in this run.
            TaskMeta *const m = g->_cur_meta;

            if (turbo::get_flag(FLAGS_show_fiber_creation_in_vars)) {
                // NOTE: the thread triggering exposure of pending time may spend
                // considerable time because a single melon::var::LatencyRecorder
                // contains many var.
                g->_control->exposed_pending_time() <<
                                                    (mutil::cpuwide_time_ns() - m->cpuwide_start_ns) / 1000L;
            }

            // Not catch exceptions except ExitException which is for implementing
            // fiber_exit(). User code is intended to crash when an exception is
            // not caught explicitly. This is consistent with other threading
            // libraries.
            void *thread_return;
            try {
                thread_return = m->fn(m->arg);
            } catch (ExitException &e) {
                thread_return = e.value();
            }

            // Group is probably changed
            g = MELON_GET_VOLATILE_THREAD_LOCAL(tls_task_group);

            // TODO: Save thread_return
            (void) thread_return;

            // Logging must be done before returning the keytable, since the logging lib
            // use fiber local storage internally, or will cause memory leak.
            // FIXME: the time from quiting fn to here is not counted into cputime
            if (m->attr.flags & FIBER_LOG_START_AND_FINISH) {
                LOG(INFO) << "Finished fiber " << m->tid << ", cputime="
                          << m->stat.cputime_ns / 1000000.0 << "ms";
            }

            // Clean tls variables, must be done before changing version_butex
            // otherwise another thread just joined this thread may not see side
            // effects of destructing tls variables.
            KeyTable *kt = tls_bls.keytable;
            if (kt != NULL) {
                return_keytable(m->attr.keytable_pool, kt);
                // After deletion: tls may be set during deletion.
                tls_bls.keytable = NULL;
                m->local_storage.keytable = NULL; // optional
            }

            // Increase the version and wake up all joiners, if resulting version
            // is 0, change it to 1 to make fiber_t never be 0. Any access
            // or join to the fiber after changing version will be rejected.
            // The spinlock is for visibility of TaskGroup::get_attr.
            {
                MELON_SCOPED_LOCK(m->version_lock);
                if (0 == ++*m->version_butex) {
                    ++*m->version_butex;
                }
            }
            butex_wake_except(m->version_butex, 0);

            g->_control->_nfibers << -1;
            g->_control->tag_nfibers(g->tag()) << -1;
            g->set_remained(TaskGroup::_release_last_context, m);
            ending_sched(&g);

        } while (g->_cur_meta->tid != g->_main_tid);

        // Was called from a pthread and we don't have FIBER_STACKTYPE_PTHREAD
        // tasks to run, quit for more tasks.
    }

    void TaskGroup::_release_last_context(void *arg) {
        TaskMeta *m = static_cast<TaskMeta *>(arg);
        if (m->stack_type() != STACK_TYPE_PTHREAD) {
            return_stack(m->release_stack()/*may be NULL*/);
        } else {
            // it's _main_stack, don't return.
            m->set_stack(NULL);
        }
        return_resource(get_slot(m->tid));
    }

    int TaskGroup::start_foreground(TaskGroup **pg,
                                    fiber_t *__restrict th,
                                    const fiber_attr_t *__restrict attr,
                                    void *(*fn)(void *),
                                    void *__restrict arg) {
        if (__builtin_expect(!fn, 0)) {
            return EINVAL;
        }
        const int64_t start_ns = mutil::cpuwide_time_ns();
        const fiber_attr_t using_attr = (attr ? *attr : FIBER_ATTR_NORMAL);
        mutil::ResourceId<TaskMeta> slot;
        TaskMeta *m = mutil::get_resource(&slot);
        if (__builtin_expect(!m, 0)) {
            return ENOMEM;
        }
        CHECK(m->current_waiter.load(std::memory_order_relaxed) == NULL);
        m->stop = false;
        m->interrupted = false;
        m->about_to_quit = false;
        m->fn = fn;
        m->arg = arg;
        CHECK(m->stack == NULL);
        m->attr = using_attr;
        m->local_storage = LOCAL_STORAGE_INIT;
        if (using_attr.flags & FIBER_INHERIT_SPAN) {
            m->local_storage.rpcz_parent_span = tls_bls.rpcz_parent_span;
        }
        m->cpuwide_start_ns = start_ns;
        m->stat = EMPTY_STAT;
        m->tid = make_tid(*m->version_butex, slot);
        *th = m->tid;
        if (using_attr.flags & FIBER_LOG_START_AND_FINISH) {
            LOG(INFO) << "Started fiber " << m->tid;
        }

        TaskGroup *g = *pg;
        g->_control->_nfibers << 1;
        g->_control->tag_nfibers(g->tag()) << 1;
        if (g->is_current_pthread_task()) {
            // never create foreground task in pthread.
            g->ready_to_run(m->tid, (using_attr.flags & FIBER_NOSIGNAL));
        } else {
            // NOSIGNAL affects current task, not the new task.
            RemainedFn fn = NULL;
            if (g->current_task()->about_to_quit) {
                fn = ready_to_run_in_worker_ignoresignal;
            } else {
                fn = ready_to_run_in_worker;
            }
            ReadyToRunArgs args = {
                    g->current_tid(),
                    (bool) (using_attr.flags & FIBER_NOSIGNAL)
            };
            g->set_remained(fn, &args);
            TaskGroup::sched_to(pg, m->tid);
        }
        return 0;
    }

    template<bool REMOTE>
    int TaskGroup::start_background(fiber_t *__restrict th,
                                    const fiber_attr_t *__restrict attr,
                                    void *(*fn)(void *),
                                    void *__restrict arg) {
        if (__builtin_expect(!fn, 0)) {
            return EINVAL;
        }
        const int64_t start_ns = mutil::cpuwide_time_ns();
        const fiber_attr_t using_attr = (attr ? *attr : FIBER_ATTR_NORMAL);
        mutil::ResourceId<TaskMeta> slot;
        TaskMeta *m = mutil::get_resource(&slot);
        if (__builtin_expect(!m, 0)) {
            return ENOMEM;
        }
        CHECK(m->current_waiter.load(std::memory_order_relaxed) == NULL);
        m->stop = false;
        m->interrupted = false;
        m->about_to_quit = false;
        m->fn = fn;
        m->arg = arg;
        CHECK(m->stack == NULL);
        m->attr = using_attr;
        m->local_storage = LOCAL_STORAGE_INIT;
        if (using_attr.flags & FIBER_INHERIT_SPAN) {
            m->local_storage.rpcz_parent_span = tls_bls.rpcz_parent_span;
        }
        m->cpuwide_start_ns = start_ns;
        m->stat = EMPTY_STAT;
        m->tid = make_tid(*m->version_butex, slot);
        *th = m->tid;
        if (using_attr.flags & FIBER_LOG_START_AND_FINISH) {
            LOG(INFO) << "Started fiber " << m->tid;
        }
        _control->_nfibers << 1;
        _control->tag_nfibers(tag()) << 1;
        if (REMOTE) {
            ready_to_run_remote(m->tid, (using_attr.flags & FIBER_NOSIGNAL));
        } else {
            ready_to_run(m->tid, (using_attr.flags & FIBER_NOSIGNAL));
        }
        return 0;
    }

// Explicit instantiations.
    template int
    TaskGroup::start_background<true>(fiber_t *__restrict th,
                                      const fiber_attr_t *__restrict attr,
                                      void *(*fn)(void *),
                                      void *__restrict arg);

    template int
    TaskGroup::start_background<false>(fiber_t *__restrict th,
                                       const fiber_attr_t *__restrict attr,
                                       void *(*fn)(void *),
                                       void *__restrict arg);

    int TaskGroup::join(fiber_t tid, void **return_value) {
        if (__builtin_expect(!tid, 0)) {  // tid of fiber is never 0.
            return EINVAL;
        }
        TaskMeta *m = address_meta(tid);
        if (__builtin_expect(!m, 0)) {
            // The fiber is not created yet, this join is definitely wrong.
            return EINVAL;
        }
        TaskGroup *g = tls_task_group;
        if (g != NULL && g->current_tid() == tid) {
            // joining self causes indefinite waiting.
            return EINVAL;
        }
        const uint32_t expected_version = get_version(tid);
        while (*m->version_butex == expected_version) {
            if (butex_wait(m->version_butex, expected_version, NULL) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return errno;
            }
        }
        if (return_value) {
            *return_value = NULL;
        }
        return 0;
    }

    bool TaskGroup::exists(fiber_t tid) {
        if (tid != 0) {  // tid of fiber is never 0.
            TaskMeta *m = address_meta(tid);
            if (m != NULL) {
                return (*m->version_butex == get_version(tid));
            }
        }
        return false;
    }

    TaskStatistics TaskGroup::main_stat() const {
        TaskMeta *m = address_meta(_main_tid);
        return m ? m->stat : EMPTY_STAT;
    }

    void TaskGroup::ending_sched(TaskGroup **pg) {
        TaskGroup *g = *pg;
        fiber_t next_tid = 0;
        // Find next task to run, if none, switch to idle thread of the group.
#ifndef FIBER_FAIR_WSQ
        // When FIBER_FAIR_WSQ is defined, profiling shows that cpu cost of
        // WSQ::steal() in example/multi_threaded_echo_c++ changes from 1.9%
        // to 2.9%
        const bool popped = g->_rq.pop(&next_tid);
#else
        const bool popped = g->_rq.steal(&next_tid);
#endif
        if (!popped && !g->steal_task(&next_tid)) {
            // Jump to main task if there's no task to run.
            next_tid = g->_main_tid;
        }

        TaskMeta *const cur_meta = g->_cur_meta;
        TaskMeta *next_meta = address_meta(next_tid);
        if (next_meta->stack == NULL) {
            if (next_meta->stack_type() == cur_meta->stack_type()) {
                // also works with pthread_task scheduling to pthread_task, the
                // transfered stack is just _main_stack.
                next_meta->set_stack(cur_meta->release_stack());
            } else {
                ContextualStack *stk = get_stack(next_meta->stack_type(), task_runner);
                if (stk) {
                    next_meta->set_stack(stk);
                } else {
                    // stack_type is FIBER_STACKTYPE_PTHREAD or out of memory,
                    // In latter case, attr is forced to be FIBER_STACKTYPE_PTHREAD.
                    // This basically means that if we can't allocate stack, run
                    // the task in pthread directly.
                    next_meta->attr.stack_type = FIBER_STACKTYPE_PTHREAD;
                    next_meta->set_stack(g->_main_stack);
                }
            }
        }
        sched_to(pg, next_meta);
    }

    void TaskGroup::sched(TaskGroup **pg) {
        TaskGroup *g = *pg;
        fiber_t next_tid = 0;
        // Find next task to run, if none, switch to idle thread of the group.
#ifndef FIBER_FAIR_WSQ
        const bool popped = g->_rq.pop(&next_tid);
#else
        const bool popped = g->_rq.steal(&next_tid);
#endif
        if (!popped && !g->steal_task(&next_tid)) {
            // Jump to main task if there's no task to run.
            next_tid = g->_main_tid;
        }
        sched_to(pg, next_tid);
    }

    void TaskGroup::sched_to(TaskGroup **pg, TaskMeta *next_meta) {
        TaskGroup *g = *pg;
#ifndef NDEBUG
        if ((++g->_sched_recursive_guard) > 1) {
            LOG(FATAL) << "Recursively(" << g->_sched_recursive_guard - 1
                       << ") call sched_to(" << g << ")";
        }
#endif
        // Save errno so that errno is fiber-specific.
        const int saved_errno = errno;
        void *saved_unique_user_ptr = tls_unique_user_ptr;

        TaskMeta *const cur_meta = g->_cur_meta;
        const int64_t now = mutil::cpuwide_time_ns();
        const int64_t elp_ns = now - g->_last_run_ns;
        g->_last_run_ns = now;
        cur_meta->stat.cputime_ns += elp_ns;
        if (cur_meta->tid != g->main_tid()) {
            g->_cumulated_cputime_ns += elp_ns;
        }
        ++cur_meta->stat.nswitch;
        ++g->_nswitch;
        // Switch to the task
        if (__builtin_expect(next_meta != cur_meta, 1)) {
            g->_cur_meta = next_meta;
            // Switch tls_bls
            cur_meta->local_storage = tls_bls;
            tls_bls = next_meta->local_storage;

            // Logging must be done after switching the local storage, since the logging lib
            // use fiber local storage internally, or will cause memory leak.
            if ((cur_meta->attr.flags & FIBER_LOG_CONTEXT_SWITCH) ||
                (next_meta->attr.flags & FIBER_LOG_CONTEXT_SWITCH)) {
                LOG(INFO) << "Switch fiber: " << cur_meta->tid << " -> "
                          << next_meta->tid;
            }

            if (cur_meta->stack != NULL) {
                if (next_meta->stack != cur_meta->stack) {
                    jump_stack(cur_meta->stack, next_meta->stack);
                    // probably went to another group, need to assign g again.
                    g = MELON_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
                }
#ifndef NDEBUG
                else {
                    // else pthread_task is switching to another pthread_task, sc
                    // can only equal when they're both _main_stack
                    CHECK(cur_meta->stack == g->_main_stack);
                }
#endif
            }
            // else because of ending_sched(including pthread_task->pthread_task)
        } else {
            LOG(FATAL) << "fiber=" << g->current_tid() << " sched_to itself!";
        }

        while (g->_last_context_remained) {
            RemainedFn fn = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = MELON_GET_VOLATILE_THREAD_LOCAL(tls_task_group);
        }

        // Restore errno
        errno = saved_errno;
        // tls_unique_user_ptr probably changed.
        MELON_SET_VOLATILE_THREAD_LOCAL(tls_unique_user_ptr, saved_unique_user_ptr);

#ifndef NDEBUG
        --g->_sched_recursive_guard;
#endif
        *pg = g;
    }

    void TaskGroup::destroy_self() {
        if (_control) {
            _control->_destroy_group(this);
            _control = NULL;
        } else {
            CHECK(false);
        }
    }

    void TaskGroup::ready_to_run(fiber_t tid, bool nosignal) {
        push_rq(tid);
        if (nosignal) {
            ++_num_nosignal;
        } else {
            const int additional_signal = _num_nosignal;
            _num_nosignal = 0;
            _nsignaled += 1 + additional_signal;
            _control->signal_task(1 + additional_signal, _tag);
        }
    }

    void TaskGroup::flush_nosignal_tasks() {
        const int val = _num_nosignal;
        if (val) {
            _num_nosignal = 0;
            _nsignaled += val;
            _control->signal_task(val, _tag);
        }
    }

    void TaskGroup::ready_to_run_remote(fiber_t tid, bool nosignal) {
        _remote_rq._mutex.lock();
        while (!_remote_rq.push_locked(tid)) {
            flush_nosignal_tasks_remote_locked(_remote_rq._mutex);
            LOG_EVERY_N_SEC(ERROR, 1) << "_remote_rq is full, capacity="
                                      << _remote_rq.capacity();
            ::usleep(1000);
            _remote_rq._mutex.lock();
        }
        if (nosignal) {
            ++_remote_num_nosignal;
            _remote_rq._mutex.unlock();
        } else {
            const int additional_signal = _remote_num_nosignal;
            _remote_num_nosignal = 0;
            _remote_nsignaled += 1 + additional_signal;
            _remote_rq._mutex.unlock();
            _control->signal_task(1 + additional_signal, _tag);
        }
    }

    void TaskGroup::flush_nosignal_tasks_remote_locked(mutil::Mutex &locked_mutex) {
        const int val = _remote_num_nosignal;
        if (!val) {
            locked_mutex.unlock();
            return;
        }
        _remote_num_nosignal = 0;
        _remote_nsignaled += val;
        locked_mutex.unlock();
        _control->signal_task(val, _tag);
    }

    void TaskGroup::ready_to_run_general(fiber_t tid, bool nosignal) {
        if (tls_task_group == this) {
            return ready_to_run(tid, nosignal);
        }
        return ready_to_run_remote(tid, nosignal);
    }

    void TaskGroup::flush_nosignal_tasks_general() {
        if (tls_task_group == this) {
            return flush_nosignal_tasks();
        }
        return flush_nosignal_tasks_remote();
    }

    void TaskGroup::ready_to_run_in_worker(void *args_in) {
        ReadyToRunArgs *args = static_cast<ReadyToRunArgs *>(args_in);
        return tls_task_group->ready_to_run(args->tid, args->nosignal);
    }

    void TaskGroup::ready_to_run_in_worker_ignoresignal(void *args_in) {
        ReadyToRunArgs *args = static_cast<ReadyToRunArgs *>(args_in);
        return tls_task_group->push_rq(args->tid);
    }

    struct SleepArgs {
        uint64_t timeout_us;
        fiber_t tid;
        TaskMeta *meta;
        TaskGroup *group;
    };

    static void ready_to_run_from_timer_thread(void *arg) {
        CHECK(tls_task_group == NULL);
        const SleepArgs *e = static_cast<const SleepArgs *>(arg);
        auto g = e->group;
        auto tag = g->tag();
        g->control()->choose_one_group(tag)->ready_to_run_remote(e->tid);
    }

    void TaskGroup::_add_sleep_event(void *void_args) {
        // Must copy SleepArgs. After calling TimerThread::schedule(), previous
        // thread may be stolen by a worker immediately and the on-stack SleepArgs
        // will be gone.
        SleepArgs e = *static_cast<SleepArgs *>(void_args);
        TaskGroup *g = e.group;

        TimerThread::TaskId sleep_id;
        sleep_id = get_global_timer_thread()->schedule(
                ready_to_run_from_timer_thread, void_args,
                mutil::microseconds_from_now(e.timeout_us));

        if (!sleep_id) {
            // fail to schedule timer, go back to previous thread.
            g->ready_to_run(e.tid);
            return;
        }

        // Set TaskMeta::current_sleep which is for interruption.
        const uint32_t given_ver = get_version(e.tid);
        {
            MELON_SCOPED_LOCK(e.meta->version_lock);
            if (given_ver == *e.meta->version_butex && !e.meta->interrupted) {
                e.meta->current_sleep = sleep_id;
                return;
            }
        }
        // The thread is stopped or interrupted.
        // interrupt() always sees that current_sleep == 0. It will not schedule
        // the calling thread. The race is between current thread and timer thread.
        if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
            // added to timer, previous thread may be already woken up by timer and
            // even stopped. It's safe to schedule previous thread when unschedule()
            // returns 0 which means "the not-run-yet sleep_id is removed". If the
            // sleep_id is running(returns 1), ready_to_run_in_worker() will
            // schedule previous thread as well. If sleep_id does not exist,
            // previous thread is scheduled by timer thread before and we don't
            // have to do it again.
            g->ready_to_run(e.tid);
        }
    }

// To be consistent with sys_usleep, set errno and return -1 on error.
    int TaskGroup::usleep(TaskGroup **pg, uint64_t timeout_us) {
        if (0 == timeout_us) {
            yield(pg);
            return 0;
        }
        TaskGroup *g = *pg;
        // We have to schedule timer after we switched to next fiber otherwise
        // the timer may wake up(jump to) current still-running context.
        SleepArgs e = {timeout_us, g->current_tid(), g->current_task(), g};
        g->set_remained(_add_sleep_event, &e);
        sched(pg);
        g = *pg;
        if (e.meta->current_sleep == 0 && !e.meta->interrupted) {
            // Fail to `_add_sleep_event'.
            errno = ESTOP;
            return -1;
        }
        e.meta->current_sleep = 0;
        if (e.meta->interrupted) {
            // Race with set and may consume multiple interruptions, which are OK.
            e.meta->interrupted = false;
            // NOTE: setting errno to ESTOP is not necessary from fiber's
            // pespective, however many RPC code expects fiber_usleep to set
            // errno to ESTOP when the thread is stopping, and print FATAL
            // otherwise. To make smooth transitions, ESTOP is still set instead
            // of EINTR when the thread is stopping.
            errno = (e.meta->stop ? ESTOP : EINTR);
            return -1;
        }
        return 0;
    }

// Defined in butex.cpp
    bool erase_from_butex_because_of_interruption(ButexWaiter *bw);

    static int interrupt_and_consume_waiters(
            fiber_t tid, ButexWaiter **pw, uint64_t *sleep_id) {
        TaskMeta *const m = TaskGroup::address_meta(tid);
        if (m == NULL) {
            return EINVAL;
        }
        const uint32_t given_ver = get_version(tid);
        MELON_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            *pw = m->current_waiter.exchange(NULL, std::memory_order_acquire);
            *sleep_id = m->current_sleep;
            m->current_sleep = 0;  // only one stopper gets the sleep_id
            m->interrupted = true;
            return 0;
        }
        return EINVAL;
    }

    static int set_butex_waiter(fiber_t tid, ButexWaiter *w) {
        TaskMeta *const m = TaskGroup::address_meta(tid);
        if (m != NULL) {
            const uint32_t given_ver = get_version(tid);
            MELON_SCOPED_LOCK(m->version_lock);
            if (given_ver == *m->version_butex) {
                // Release fence makes m->interrupted visible to butex_wait
                m->current_waiter.store(w, std::memory_order_release);
                return 0;
            }
        }
        return EINVAL;
    }

// The interruption is "persistent" compared to the ones caused by signals,
// namely if a fiber is interrupted when it's not blocked, the interruption
// is still remembered and will be checked at next blocking. This designing
// choice simplifies the implementation and reduces notification loss caused
// by race conditions.
// TODO: fibers created by FIBER_ATTR_PTHREAD blocking on fiber_usleep()
// can't be interrupted.
    int TaskGroup::interrupt(fiber_t tid, TaskControl *c) {
        // Consume current_waiter in the TaskMeta, wake it up then set it back.
        ButexWaiter *w = NULL;
        uint64_t sleep_id = 0;
        int rc = interrupt_and_consume_waiters(tid, &w, &sleep_id);
        if (rc) {
            return rc;
        }
        // a fiber cannot wait on a butex and be sleepy at the same time.
        CHECK(!sleep_id || !w);
        if (w != NULL) {
            erase_from_butex_because_of_interruption(w);
            // If butex_wait() already wakes up before we set current_waiter back,
            // the function will spin until current_waiter becomes non-NULL.
            rc = set_butex_waiter(tid, w);
            if (rc) {
                LOG(FATAL) << "butex_wait should spin until setting back waiter";
                return rc;
            }
        } else if (sleep_id != 0) {
            if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
                fiber::TaskGroup *g = fiber::tls_task_group;
                if (g) {
                    g->ready_to_run(tid);
                } else {
                    if (!c) {
                        return EINVAL;
                    }
                    c->choose_one_group()->ready_to_run_remote(tid);
                }
            }
        }
        return 0;
    }

    void TaskGroup::yield(TaskGroup **pg) {
        TaskGroup *g = *pg;
        ReadyToRunArgs args = {g->current_tid(), false};
        g->set_remained(ready_to_run_in_worker, &args);
        sched(pg);
    }

    void print_task(std::ostream &os, fiber_t tid) {
        TaskMeta *const m = TaskGroup::address_meta(tid);
        if (m == NULL) {
            os << "fiber=" << tid << " : never existed";
            return;
        }
        const uint32_t given_ver = get_version(tid);
        bool matched = false;
        bool stop = false;
        bool interrupted = false;
        bool about_to_quit = false;
        void *(*fn)(void *) = NULL;
        void *arg = NULL;
        fiber_attr_t attr = FIBER_ATTR_NORMAL;
        bool has_tls = false;
        int64_t cpuwide_start_ns = 0;
        TaskStatistics stat = {0, 0};
        {
            MELON_SCOPED_LOCK(m->version_lock);
            if (given_ver == *m->version_butex) {
                matched = true;
                stop = m->stop;
                interrupted = m->interrupted;
                about_to_quit = m->about_to_quit;
                fn = m->fn;
                arg = m->arg;
                attr = m->attr;
                has_tls = m->local_storage.keytable;
                cpuwide_start_ns = m->cpuwide_start_ns;
                stat = m->stat;
            }
        }
        if (!matched) {
            os << "fiber=" << tid << " : not exist now";
        } else {
            os << "fiber=" << tid << " :\nstop=" << stop
               << "\ninterrupted=" << interrupted
               << "\nabout_to_quit=" << about_to_quit
               << "\nfn=" << (void *) fn
               << "\narg=" << (void *) arg
               << "\nattr={stack_type=" << attr.stack_type
               << " flags=" << attr.flags
               << " keytable_pool=" << attr.keytable_pool
               << "}\nhas_tls=" << has_tls
               << "\nuptime_ns=" << mutil::cpuwide_time_ns() - cpuwide_start_ns
               << "\ncputime_ns=" << stat.cputime_ns
               << "\nnswitch=" << stat.nswitch;
        }
    }

}  // namespace fiber
