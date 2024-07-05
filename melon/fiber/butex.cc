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


#include <atomic>
#include <melon/utility/scoped_lock.h>              // MELON_SCOPED_LOCK
#include <melon/utility/macros.h>
#include <melon/utility/containers/linked_list.h>   // LinkNode

#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
#include <melon/utility/memory/singleton_on_pthread_once.h>
#endif

#include <turbo/log/logging.h>
#include <melon/utility/object_pool.h>
#include <melon/fiber/errno.h>                 // EWOULDBLOCK
#include <melon/fiber/sys_futex.h>             // futex_*
#include <melon/fiber/processor.h>             // cpu_relax
#include <melon/fiber/task_control.h>          // TaskControl
#include <melon/fiber/task_group.h>            // TaskGroup
#include <melon/fiber/timer_thread.h>
#include <melon/fiber/butex.h>
#include <melon/fiber/mutex.h>

// This file implements butex.h
// Provides futex-like semantics which is sequenced wait and wake operations
// and guaranteed visibilities.
//
// If wait is sequenced before wake:
//    [thread1]             [thread2]
//    wait()                value = new_value
//                          wake()
// wait() sees unmatched value(fail to wait), or wake() sees the waiter.
//
// If wait is sequenced after wake:
//    [thread1]             [thread2]
//                          value = new_value
//                          wake()
//    wait()
// wake() must provide some sort of memory fence to prevent assignment
// of value to be reordered after it. Thus the value is visible to wait()
// as well.

namespace fiber {

#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
    struct ButexWaiterCount : public melon::var::Adder<int64_t> {
        ButexWaiterCount() : melon::var::Adder<int64_t>("fiber_butex_waiter_count") {}
    };
    inline melon::var::Adder<int64_t>& butex_waiter_count() {
        return *mutil::get_leaky_singleton<ButexWaiterCount>();
    }
#endif

    // If a thread would suspend for less than so many microseconds, return
    // ETIMEDOUT directly.
    // Use 1: sleeping for less than 2 microsecond is inefficient and useless.
    static const int64_t MIN_SLEEP_US = 2;

    enum WaiterState {
        WAITER_STATE_NONE,
        WAITER_STATE_READY,
        WAITER_STATE_TIMEDOUT,
        WAITER_STATE_UNMATCHEDVALUE,
        WAITER_STATE_INTERRUPTED,
    };

    struct Butex;

    struct ButexWaiter : public mutil::LinkNode<ButexWaiter> {
        // tids of pthreads are 0
        fiber_t tid;

        // Erasing node from middle of LinkedList is thread-unsafe, we need
        // to hold its container's lock.
        std::atomic<Butex *> container;
    };

    // non_pthread_task allocates this structure on stack and queue it in
    // Butex::waiters.
    struct ButexFiberWaiter : public ButexWaiter {
        TaskMeta *task_meta;
        TimerThread::TaskId sleep_id;
        WaiterState waiter_state;
        int expected_value;
        Butex *initial_butex;
        TaskControl *control;
        const timespec *abstime;
    };

// pthread_task or main_task allocates this structure on stack and queue it
// in Butex::waiters.
    struct ButexPthreadWaiter : public ButexWaiter {
        std::atomic<int> sig;
    };

    typedef mutil::LinkedList<ButexWaiter> ButexWaiterList;

    enum ButexPthreadSignal {
        PTHREAD_NOT_SIGNALLED, PTHREAD_SIGNALLED
    };

    struct MELON_CACHELINE_ALIGNMENT Butex {
        Butex() {}

        ~Butex() {}

        std::atomic<int> value;
        ButexWaiterList waiters;
        internal::FastPthreadMutex waiter_lock;
    };

    MELON_CASSERT(offsetof(Butex, value) == 0, offsetof_value_must_0);
    MELON_CASSERT(sizeof(Butex) == MELON_CACHELINE_SIZE, butex_fits_in_one_cacheline);

    static void wakeup_pthread(ButexPthreadWaiter *pw) {
        // release fence makes wait_pthread see changes before wakeup.
        pw->sig.store(PTHREAD_SIGNALLED, std::memory_order_release);
        // At this point, wait_pthread() possibly has woken up and destroyed `pw'.
        // In which case, futex_wake_private() should return EFAULT.
        // If crash happens in future, `pw' can be made TLS and never destroyed
        // to solve the issue.
        futex_wake_private(&pw->sig, 1);
    }

    bool erase_from_butex(ButexWaiter *, bool, WaiterState);

    int wait_pthread(ButexPthreadWaiter &pw, const timespec *abstime) {
        timespec *ptimeout = nullptr;
        timespec timeout;
        int64_t timeout_us = 0;
        int rc;

        while (true) {
            if (abstime != nullptr) {
                timeout_us = mutil::timespec_to_microseconds(*abstime) - mutil::gettimeofday_us();
                timeout = mutil::microseconds_to_timespec(timeout_us);
                ptimeout = &timeout;
            }
            if (timeout_us > MIN_SLEEP_US || abstime == nullptr) {
                rc = futex_wait_private(&pw.sig, PTHREAD_NOT_SIGNALLED, ptimeout);
                if (PTHREAD_NOT_SIGNALLED != pw.sig.load(std::memory_order_acquire)) {
                    // If `sig' is changed, wakeup_pthread() must be called and `pw'
                    // is already removed from the butex.
                    // Acquire fence makes this thread sees changes before wakeup.
                    return rc;
                }
            } else {
                errno = ETIMEDOUT;
                rc = -1;
            }
            // Handle ETIMEDOUT when abstime is valid.
            // If futex_wait_private return EINTR, just continue the loop.
            if (rc != 0 && errno == ETIMEDOUT) {
                // wait futex timeout, `pw' is still in the queue, remove it.
                if (!erase_from_butex(&pw, false, WAITER_STATE_TIMEDOUT)) {
                    // Another thread is erasing `pw' as well, wait for the signal.
                    // Acquire fence makes this thread sees changes before wakeup.
                    if (pw.sig.load(std::memory_order_acquire) == PTHREAD_NOT_SIGNALLED) {
                        // already timedout, abstime and ptimeout are expired.
                        abstime = nullptr;
                        ptimeout = nullptr;
                        continue;
                    }
                }
                return rc;
            }
        }
    }

    extern MELON_THREAD_LOCAL TaskGroup *tls_task_group;
    extern MELON_THREAD_LOCAL TaskGroup *tls_task_group_nosignal;

    // Returns 0 when no need to unschedule or successfully unscheduled,
    // -1 otherwise.
    inline int unsleep_if_necessary(ButexFiberWaiter *w,
                                    TimerThread *timer_thread) {
        if (!w->sleep_id) {
            return 0;
        }
        if (timer_thread->unschedule(w->sleep_id) > 0) {
            // the callback is running.
            return -1;
        }
        w->sleep_id = 0;
        return 0;
    }

    // Use ObjectPool(which never frees memory) to solve the race between
    // butex_wake() and butex_destroy(). The race is as follows:
    //
    //   class Event {
    //   public:
    //     void wait() {
    //       _mutex.lock();
    //       if (!_done) {
    //         _cond.wait(&_mutex);
    //       }
    //       _mutex.unlock();
    //     }
    //     void signal() {
    //       _mutex.lock();
    //       if (!_done) {
    //         _done = true;
    //         _cond.signal();
    //       }
    //       _mutex.unlock();  /*1*/
    //     }
    //   private:
    //     bool _done = false;
    //     Mutex _mutex;
    //     Condition _cond;
    //   };
    //
    //   [Thread1]                         [Thread2]
    //   foo() {
    //     Event event;
    //     pass_to_thread2(&event);  --->  event.signal();
    //     event.wait();
    //   } <-- event destroyed
    //
    // Summary: Thread1 passes a stateful condition to Thread2 and waits until
    // the condition being signalled, which basically means the associated
    // job is done and Thread1 can release related resources including the mutex
    // and condition. The scenario is fine and the code is correct.
    // The race needs a closer look. The unlock at /*1*/ may have different
    // implementations, but in which the last step is probably an atomic store
    // and butex_wake(), like this:
    //
    //   locked->store(0);
    //   butex_wake(locked);
    //
    // The `locked' represents the locking status of the mutex. The issue is that
    // just after the store(), the mutex is already unlocked and the code in
    // Event.wait() may successfully grab the lock and go through everything
    // left and leave foo() function, destroying the mutex and butex, making
    // the butex_wake(locked) crash.
    // To solve this issue, one method is to add reference before store and
    // release the reference after butex_wake. However reference countings need
    // to be added in nearly every user scenario of butex_wake(), which is very
    // error-prone. Another method is never freeing butex, with the side effect
    // that butex_wake() may wake up an unrelated butex(the one reuses the memory)
    // and cause spurious wakeups. According to our observations, the race is
    // infrequent, even rare. The extra spurious wakeups should be acceptable.

    void *butex_create() {
        Butex *b = mutil::get_object<Butex>();
        if (b) {
            return &b->value;
        }
        return nullptr;
    }

    void butex_destroy(void *butex) {
        if (!butex) {
            return;
        }
        Butex *b = static_cast<Butex *>(
                container_of(static_cast<std::atomic<int> *>(butex), Butex, value));
        mutil::return_object(b);
    }

    inline TaskGroup *get_task_group(TaskControl *c, bool nosignal = false) {
        TaskGroup *g = tls_task_group;
        if (nosignal) {
            if (nullptr == tls_task_group_nosignal) {
                g = g ? g : c->choose_one_group();
                tls_task_group_nosignal = g;
            } else {
                g = tls_task_group_nosignal;
            }
        } else {
            g = g ? g : c->choose_one_group();
        }
        return g;
    }

    inline void run_in_local_task_group(TaskGroup *g, fiber_t tid, bool nosignal) {
        if (!nosignal) {
            TaskGroup::exchange(&g, tid);
        } else {
            g->ready_to_run(tid, nosignal);
        }
    }

    int butex_wake(void *arg, bool nosignal) {
        Butex *b = container_of(static_cast<std::atomic<int> *>(arg), Butex, value);
        ButexWaiter *front = nullptr;
        {
            MELON_SCOPED_LOCK(b->waiter_lock);
            if (b->waiters.empty()) {
                return 0;
            }
            front = b->waiters.head()->value();
            front->RemoveFromList();
            front->container.store(nullptr, std::memory_order_relaxed);
        }
        if (front->tid == 0) {
            wakeup_pthread(static_cast<ButexPthreadWaiter *>(front));
            return 1;
        }
        ButexFiberWaiter *bbw = static_cast<ButexFiberWaiter *>(front);
        unsleep_if_necessary(bbw, get_global_timer_thread());
        TaskGroup *g = get_task_group(bbw->control, nosignal);
        if (g == tls_task_group) {
            run_in_local_task_group(g, bbw->tid, nosignal);
        } else {
            g->ready_to_run_remote(bbw->tid, nosignal);
        }
        return 1;
    }

    int butex_wake_all(void *arg, bool nosignal) {
        Butex *b = container_of(static_cast<std::atomic<int> *>(arg), Butex, value);

        ButexWaiterList fiber_waiters;
        ButexWaiterList pthread_waiters;
        {
            MELON_SCOPED_LOCK(b->waiter_lock);
            while (!b->waiters.empty()) {
                ButexWaiter *bw = b->waiters.head()->value();
                bw->RemoveFromList();
                bw->container.store(nullptr, std::memory_order_relaxed);
                if (bw->tid) {
                    fiber_waiters.Append(bw);
                } else {
                    pthread_waiters.Append(bw);
                }
            }
        }

        int nwakeup = 0;
        while (!pthread_waiters.empty()) {
            ButexPthreadWaiter *bw = static_cast<ButexPthreadWaiter *>(
                    pthread_waiters.head()->value());
            bw->RemoveFromList();
            wakeup_pthread(bw);
            ++nwakeup;
        }
        if (fiber_waiters.empty()) {
            return nwakeup;
        }
        // We will exchange with first waiter in the end.
        ButexFiberWaiter *next = static_cast<ButexFiberWaiter *>(
                fiber_waiters.head()->value());
        next->RemoveFromList();
        unsleep_if_necessary(next, get_global_timer_thread());
        ++nwakeup;
        TaskGroup *g = get_task_group(next->control, nosignal);
        const int saved_nwakeup = nwakeup;
        while (!fiber_waiters.empty()) {
            // pop reversely
            ButexFiberWaiter *w = static_cast<ButexFiberWaiter *>(
                    fiber_waiters.tail()->value());
            w->RemoveFromList();
            unsleep_if_necessary(w, get_global_timer_thread());
            g->ready_to_run_general(w->tid, true);
            ++nwakeup;
        }
        if (!nosignal && saved_nwakeup != nwakeup) {
            g->flush_nosignal_tasks_general();
        }
        if (g == tls_task_group) {
            run_in_local_task_group(g, next->tid, nosignal);
        } else {
            g->ready_to_run_remote(next->tid, nosignal);
        }
        return nwakeup;
    }

    int butex_wake_except(void *arg, fiber_t excluded_fiber) {
        Butex *b = container_of(static_cast<std::atomic<int> *>(arg), Butex, value);

        ButexWaiterList fiber_waiters;
        ButexWaiterList pthread_waiters;
        {
            ButexWaiter *excluded_waiter = nullptr;
            MELON_SCOPED_LOCK(b->waiter_lock);
            while (!b->waiters.empty()) {
                ButexWaiter *bw = b->waiters.head()->value();
                bw->RemoveFromList();

                if (bw->tid) {
                    if (bw->tid != excluded_fiber) {
                        fiber_waiters.Append(bw);
                        bw->container.store(nullptr, std::memory_order_relaxed);
                    } else {
                        excluded_waiter = bw;
                    }
                } else {
                    bw->container.store(nullptr, std::memory_order_relaxed);
                    pthread_waiters.Append(bw);
                }
            }

            if (excluded_waiter) {
                b->waiters.Append(excluded_waiter);
            }
        }

        int nwakeup = 0;
        while (!pthread_waiters.empty()) {
            ButexPthreadWaiter *bw = static_cast<ButexPthreadWaiter *>(
                    pthread_waiters.head()->value());
            bw->RemoveFromList();
            wakeup_pthread(bw);
            ++nwakeup;
        }

        if (fiber_waiters.empty()) {
            return nwakeup;
        }
        ButexFiberWaiter *front = static_cast<ButexFiberWaiter *>(
                fiber_waiters.head()->value());

        TaskGroup *g = get_task_group(front->control);
        const int saved_nwakeup = nwakeup;
        do {
            // pop reversely
            ButexFiberWaiter *w = static_cast<ButexFiberWaiter *>(
                    fiber_waiters.tail()->value());
            w->RemoveFromList();
            unsleep_if_necessary(w, get_global_timer_thread());
            g->ready_to_run_general(w->tid, true);
            ++nwakeup;
        } while (!fiber_waiters.empty());
        if (saved_nwakeup != nwakeup) {
            g->flush_nosignal_tasks_general();
        }
        return nwakeup;
    }

    int butex_requeue(void *arg, void *arg2) {
        Butex *b = container_of(static_cast<std::atomic<int> *>(arg), Butex, value);
        Butex *m = container_of(static_cast<std::atomic<int> *>(arg2), Butex, value);

        ButexWaiter *front = nullptr;
        {
            std::unique_lock<internal::FastPthreadMutex> lck1(b->waiter_lock, std::defer_lock);
            std::unique_lock<internal::FastPthreadMutex> lck2(m->waiter_lock, std::defer_lock);
            mutil::double_lock(lck1, lck2);
            if (b->waiters.empty()) {
                return 0;
            }

            front = b->waiters.head()->value();
            front->RemoveFromList();
            front->container.store(nullptr, std::memory_order_relaxed);

            while (!b->waiters.empty()) {
                ButexWaiter *bw = b->waiters.head()->value();
                bw->RemoveFromList();
                m->waiters.Append(bw);
                bw->container.store(m, std::memory_order_relaxed);
            }
        }

        if (front->tid == 0) {  // which is a pthread
            wakeup_pthread(static_cast<ButexPthreadWaiter *>(front));
            return 1;
        }
        ButexFiberWaiter *bbw = static_cast<ButexFiberWaiter *>(front);
        unsleep_if_necessary(bbw, get_global_timer_thread());
        TaskGroup *g = tls_task_group;
        if (g) {
            TaskGroup::exchange(&g, front->tid);
        } else {
            bbw->control->choose_one_group()->ready_to_run_remote(front->tid);
        }
        return 1;
    }

// Callable from multiple threads, at most one thread may wake up the waiter.
    static void erase_from_butex_and_wakeup(void *arg) {
        erase_from_butex(static_cast<ButexWaiter *>(arg), true, WAITER_STATE_TIMEDOUT);
    }

// Used in task_group.cpp
    bool erase_from_butex_because_of_interruption(ButexWaiter *bw) {
        return erase_from_butex(bw, true, WAITER_STATE_INTERRUPTED);
    }

    inline bool erase_from_butex(ButexWaiter *bw, bool wakeup, WaiterState state) {
        // `bw' is guaranteed to be valid inside this function because waiter
        // will wait until this function being cancelled or finished.
        // NOTE: This function must be no-op when bw->container is nullptr.
        bool erased = false;
        Butex *b;
        int saved_errno = errno;
        while ((b = bw->container.load(std::memory_order_acquire))) {
            // b can be nullptr when the waiter is scheduled but queued.
            MELON_SCOPED_LOCK(b->waiter_lock);
            if (b == bw->container.load(std::memory_order_relaxed)) {
                bw->RemoveFromList();
                bw->container.store(nullptr, std::memory_order_relaxed);
                if (bw->tid) {
                    static_cast<ButexFiberWaiter *>(bw)->waiter_state = state;
                }
                erased = true;
                break;
            }
        }
        if (erased && wakeup) {
            if (bw->tid) {
                ButexFiberWaiter *bbw = static_cast<ButexFiberWaiter *>(bw);
                get_task_group(bbw->control)->ready_to_run_general(bw->tid);
            } else {
                ButexPthreadWaiter *pw = static_cast<ButexPthreadWaiter *>(bw);
                wakeup_pthread(pw);
            }
        }
        errno = saved_errno;
        return erased;
    }

    static void wait_for_butex(void *arg) {
        ButexFiberWaiter *const bw = static_cast<ButexFiberWaiter *>(arg);
        Butex *const b = bw->initial_butex;
        // 1: waiter with timeout should have waiter_state == WAITER_STATE_READY
        //    before they're queued, otherwise the waiter is already timedout
        //    and removed by TimerThread, in which case we should stop queueing.
        //
        // Visibility of waiter_state:
        //    [fiber]                         [TimerThread]
        //    waiter_state = TIMED
        //    tt_lock { add task }
        //                                      tt_lock { get task }
        //                                      waiter_lock { waiter_state=TIMEDOUT }
        //    waiter_lock { use waiter_state }
        // tt_lock represents TimerThread::_mutex. Visibility of waiter_state is
        // sequenced by two locks, both threads are guaranteed to see the correct
        // value.
        {
            MELON_SCOPED_LOCK(b->waiter_lock);
            if (b->value.load(std::memory_order_relaxed) != bw->expected_value) {
                bw->waiter_state = WAITER_STATE_UNMATCHEDVALUE;
            } else if (bw->waiter_state == WAITER_STATE_READY/*1*/ &&
                       !bw->task_meta->interrupted) {
                b->waiters.Append(bw);
                bw->container.store(b, std::memory_order_relaxed);
                if (bw->abstime != nullptr) {
                    bw->sleep_id = get_global_timer_thread()->schedule(
                            erase_from_butex_and_wakeup, bw, *bw->abstime);
                    if (!bw->sleep_id) {  // TimerThread stopped.
                        errno = ESTOP;
                        erase_from_butex_and_wakeup(bw);
                    }
                }
                return;
            }
        }

        // b->container is nullptr which makes erase_from_butex_and_wakeup() and
        // TaskGroup::interrupt() no-op, there's no race between following code and
        // the two functions. The on-stack ButexFiberWaiter is safe to use and
        // bw->waiter_state will not change again.
        // unsleep_if_necessary(bw, get_global_timer_thread());
        tls_task_group->ready_to_run(bw->tid);
        // FIXME: jump back to original thread is buggy.

        // // Value unmatched or waiter is already woken up by TimerThread, jump
        // // back to original fiber.
        // TaskGroup* g = tls_task_group;
        // ReadyToRunArgs args = { g->current_tid(), false };
        // g->set_remained(TaskGroup::ready_to_run_in_worker, &args);
        // // 2: Don't run remained because we're already in a remained function
        // //    otherwise stack may overflow.
        // TaskGroup::sched_to(&g, bw->tid, false/*2*/);
    }

    static int butex_wait_from_pthread(TaskGroup *g, Butex *b, int expected_value,
                                       const timespec *abstime) {
        TaskMeta *task = nullptr;
        ButexPthreadWaiter pw;
        pw.tid = 0;
        pw.sig.store(PTHREAD_NOT_SIGNALLED, std::memory_order_relaxed);
        int rc = 0;

        if (g) {
            task = g->current_task();
            task->current_waiter.store(&pw, std::memory_order_release);
        }
        b->waiter_lock.lock();
        if (b->value.load(std::memory_order_relaxed) != expected_value) {
            b->waiter_lock.unlock();
            errno = EWOULDBLOCK;
            rc = -1;
        } else if (task != nullptr && task->interrupted) {
            b->waiter_lock.unlock();
            // Race with set and may consume multiple interruptions, which are OK.
            task->interrupted = false;
            errno = EINTR;
            rc = -1;
        } else {
            b->waiters.Append(&pw);
            pw.container.store(b, std::memory_order_relaxed);
            b->waiter_lock.unlock();

#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
            melon::var::Adder<int64_t>& num_waiters = butex_waiter_count();
            num_waiters << 1;
#endif
            rc = wait_pthread(pw, abstime);
#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
            num_waiters << -1;
#endif
        }
        if (task) {
            // If current_waiter is nullptr, TaskGroup::interrupt() is running and
            // using pw, spin until current_waiter != nullptr.
            BT_LOOP_WHEN(task->current_waiter.exchange(
                    nullptr, std::memory_order_acquire) == nullptr,
                         30/*nops before sched_yield*/);
            if (task->interrupted) {
                task->interrupted = false;
                if (rc == 0) {
                    errno = EINTR;
                    return -1;
                }
            }
        }
        return rc;
    }

    int butex_wait(void *arg, int expected_value, const timespec *abstime) {
        Butex *b = container_of(static_cast<std::atomic<int> *>(arg), Butex, value);
        if (b->value.load(std::memory_order_relaxed) != expected_value) {
            errno = EWOULDBLOCK;
            // Sometimes we may take actions immediately after unmatched butex,
            // this fence makes sure that we see changes before changing butex.
            std::atomic_thread_fence(std::memory_order_acquire);
            return -1;
        }
        TaskGroup *g = tls_task_group;
        if (nullptr == g || g->is_current_pthread_task()) {
            return butex_wait_from_pthread(g, b, expected_value, abstime);
        }
        ButexFiberWaiter bbw;
        // tid is 0 iff the thread is non-fiber
        bbw.tid = g->current_tid();
        bbw.container.store(nullptr, std::memory_order_relaxed);
        bbw.task_meta = g->current_task();
        bbw.sleep_id = 0;
        bbw.waiter_state = WAITER_STATE_READY;
        bbw.expected_value = expected_value;
        bbw.initial_butex = b;
        bbw.control = g->control();
        bbw.abstime = abstime;

        if (abstime != nullptr) {
            // Schedule timer before queueing. If the timer is triggered before
            // queueing, cancel queueing. This is a kind of optimistic locking.
            if (mutil::timespec_to_microseconds(*abstime) <
                (mutil::gettimeofday_us() + MIN_SLEEP_US)) {
                // Already timed out.
                errno = ETIMEDOUT;
                return -1;
            }
        }
#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
        melon::var::Adder<int64_t>& num_waiters = butex_waiter_count();
        num_waiters << 1;
#endif

        // release fence matches with acquire fence in interrupt_and_consume_waiters
        // in task_group.cpp to guarantee visibility of `interrupted'.
        bbw.task_meta->current_waiter.store(&bbw, std::memory_order_release);
        g->set_remained(wait_for_butex, &bbw);
        TaskGroup::sched(&g);

        // erase_from_butex_and_wakeup (called by TimerThread) is possibly still
        // running and using bbw. The chance is small, just spin until it's done.
        BT_LOOP_WHEN(unsleep_if_necessary(&bbw, get_global_timer_thread()) < 0,
                     30/*nops before sched_yield*/);

        // If current_waiter is nullptr, TaskGroup::interrupt() is running and using bbw.
        // Spin until current_waiter != nullptr.
        BT_LOOP_WHEN(bbw.task_meta->current_waiter.exchange(
                nullptr, std::memory_order_acquire) == nullptr,
                     30/*nops before sched_yield*/);
#ifdef SHOW_FIBER_BUTEX_WAITER_COUNT_IN_VARS
        num_waiters << -1;
#endif

        bool is_interrupted = false;
        if (bbw.task_meta->interrupted) {
            // Race with set and may consume multiple interruptions, which are OK.
            bbw.task_meta->interrupted = false;
            is_interrupted = true;
        }
        // If timed out as well as value unmatched, return ETIMEDOUT.
        if (WAITER_STATE_TIMEDOUT == bbw.waiter_state) {
            errno = ETIMEDOUT;
            return -1;
        } else if (WAITER_STATE_UNMATCHEDVALUE == bbw.waiter_state) {
            errno = EWOULDBLOCK;
            return -1;
        } else if (is_interrupted) {
            errno = EINTR;
            return -1;
        }
        return 0;
    }

}  // namespace fiber

namespace mutil {
    template<>
    struct ObjectPoolBlockMaxItem<fiber::Butex> {
        static const size_t value = 128;
    };
}
