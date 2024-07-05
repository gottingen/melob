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


#include <melon/utility/scoped_lock.h>             // MELON_SCOPED_LOCK
#include <melon/utility/errno.h>                   // berror
#include <turbo/log/logging.h>
#include <melon/utility/threading/platform_thread.h>
#include <melon/utility/third_party/murmurhash3/murmurhash3.h>
#include <melon/fiber/sys_futex.h>            // futex_wake_private
#include <melon/fiber/interrupt_pthread.h>
#include <melon/fiber/processor.h>            // cpu_relax
#include <melon/fiber/task_group.h>           // TaskGroup
#include <melon/fiber/task_control.h>
#include <melon/fiber/timer_thread.h>         // global_timer_thread
#include <melon/fiber/log.h>
#include <turbo/flags/flag.h>

TURBO_FLAG(int32_t, task_group_delete_delay, 1,
           "delay deletion of TaskGroup for so many seconds");
TURBO_FLAG(int32_t, task_group_runqueue_capacity, 4096,
           "capacity of runqueue in each TaskGroup");
TURBO_FLAG(int32_t, task_group_yield_before_idle, 0,
           "TaskGroup yields so many times before idle");
TURBO_FLAG(int32_t, task_group_ntags, 1, "TaskGroup will be grouped by number ntags");

namespace fiber {

    extern pthread_mutex_t g_task_control_mutex;
    extern MELON_THREAD_LOCAL TaskGroup *tls_task_group;

    void (*g_worker_startfn)() = NULL;

    void (*g_tagged_worker_startfn)(fiber_tag_t) = NULL;

    // May be called in other modules to run startfn in non-worker pthreads.
    void run_worker_startfn() {
        if (g_worker_startfn) {
            g_worker_startfn();
        }
    }

    void run_tagged_worker_startfn(fiber_tag_t tag) {
        if (g_tagged_worker_startfn) {
            g_tagged_worker_startfn(tag);
        }
    }

    struct WorkerThreadArgs {
        WorkerThreadArgs(TaskControl *_c, fiber_tag_t _t) : c(_c), tag(_t) {}

        TaskControl *c;
        fiber_tag_t tag;
    };

    void *TaskControl::worker_thread(void *arg) {
        run_worker_startfn();

        auto dummy = static_cast<WorkerThreadArgs *>(arg);
        auto c = dummy->c;
        auto tag = dummy->tag;
        delete dummy;
        run_tagged_worker_startfn(tag);

        TaskGroup *g = c->create_group(tag);
        TaskStatistics stat;
        if (NULL == g) {
            LOG(ERROR) << "Fail to create TaskGroup in pthread=" << pthread_self();
            return NULL;
        }
        std::string worker_thread_name = mutil::string_printf(
                "melon_wkr:%d-%d", g->tag(), c->_next_worker_id.fetch_add(1, std::memory_order_relaxed));
        mutil::PlatformThread::SetName(worker_thread_name.c_str());
        BT_VLOG << "Created worker=" << pthread_self() << " fiber=" << g->main_tid()
                << " tag=" << g->tag();
        tls_task_group = g;
        c->_nworkers << 1;
        c->tag_nworkers(g->tag()) << 1;
        g->run_main_task();

        stat = g->main_stat();
        BT_VLOG << "Destroying worker=" << pthread_self() << " fiber="
                << g->main_tid() << " idle=" << stat.cputime_ns / 1000000.0
                << "ms uptime=" << g->current_uptime_ns() / 1000000.0 << "ms";
        tls_task_group = NULL;
        g->destroy_self();
        c->_nworkers << -1;
        c->tag_nworkers(g->tag()) << -1;
        return NULL;
    }

    TaskGroup *TaskControl::create_group(fiber_tag_t tag) {
        TaskGroup *g = new(std::nothrow) TaskGroup(this);
        if (NULL == g) {
            LOG(FATAL) << "Fail to new TaskGroup";
            return NULL;
        }
        if (g->init(turbo::get_flag(FLAGS_task_group_runqueue_capacity)) != 0) {
            LOG(ERROR) << "Fail to init TaskGroup";
            delete g;
            return NULL;
        }
        if (_add_group(g, tag) != 0) {
            delete g;
            return NULL;
        }
        return g;
    }

    static void print_rq_sizes_in_the_tc(std::ostream &os, void *arg) {
        TaskControl *tc = (TaskControl *) arg;
        tc->print_rq_sizes(os);
    }

    static double get_cumulated_worker_time_from_this(void *arg) {
        return static_cast<TaskControl *>(arg)->get_cumulated_worker_time();
    }

    struct CumulatedWithTagArgs {
        CumulatedWithTagArgs(TaskControl *_c, fiber_tag_t _t) : c(_c), t(_t) {}

        TaskControl *c;
        fiber_tag_t t;
    };

    static double get_cumulated_worker_time_from_this_with_tag(void *arg) {
        auto a = static_cast<CumulatedWithTagArgs *>(arg);
        auto c = a->c;
        auto t = a->t;
        return c->get_cumulated_worker_time_with_tag(t);
    }

    static int64_t get_cumulated_switch_count_from_this(void *arg) {
        return static_cast<TaskControl *>(arg)->get_cumulated_switch_count();
    }

    static int64_t get_cumulated_signal_count_from_this(void *arg) {
        return static_cast<TaskControl *>(arg)->get_cumulated_signal_count();
    }

    TaskControl::TaskControl()
    // NOTE: all fileds must be initialized before the vars.
            : _tagged_ngroup(turbo::get_flag(FLAGS_task_group_ntags)),
              _tagged_groups(turbo::get_flag(FLAGS_task_group_ntags)), _init(false),
              _stop(false), _concurrency(0), _next_worker_id(0), _nworkers("fiber_worker_count"), _pending_time(NULL)
            // Delay exposure of following two vars because they rely on TC which
            // is not initialized yet.
            , _cumulated_worker_time(get_cumulated_worker_time_from_this, this),
              _worker_usage_second(&_cumulated_worker_time, 1),
              _cumulated_switch_count(get_cumulated_switch_count_from_this, this),
              _switch_per_second(&_cumulated_switch_count),
              _cumulated_signal_count(get_cumulated_signal_count_from_this, this),
              _signal_per_second(&_cumulated_signal_count), _status(print_rq_sizes_in_the_tc, this),
              _nfibers("fiber_count"), _pl(turbo::get_flag(FLAGS_task_group_ntags)) {}

    int TaskControl::init(int concurrency) {
        if (_concurrency != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (concurrency <= 0) {
            LOG(ERROR) << "Invalid concurrency=" << concurrency;
            return -1;
        }
        _concurrency = concurrency;

        // task group group by tags
        for (int i = 0; i < turbo::get_flag(FLAGS_task_group_ntags); ++i) {
            _tagged_ngroup[i].store(0, std::memory_order_relaxed);
            auto tag_str = std::to_string(i);
            _tagged_nworkers.push_back(new melon::var::Adder<int64_t>("fiber_worker_count", tag_str));
            _tagged_cumulated_worker_time.push_back(new melon::var::PassiveStatus<double>(
                    get_cumulated_worker_time_from_this_with_tag, new CumulatedWithTagArgs{this, i}));
            _tagged_worker_usage_second.push_back(new melon::var::PerSecond<melon::var::PassiveStatus<double>>(
                    "fiber_worker_usage", tag_str, _tagged_cumulated_worker_time[i], 1));
            _tagged_nfibers.push_back(new melon::var::Adder<int64_t>("fiber_count", tag_str));
        }

        // Make sure TimerThread is ready.
        if (get_or_create_global_timer_thread() == NULL) {
            LOG(ERROR) << "Fail to get global_timer_thread";
            return -1;
        }

        _workers.resize(_concurrency);
        for (int i = 0; i < _concurrency; ++i) {
            auto arg = new WorkerThreadArgs(this, i % turbo::get_flag(FLAGS_task_group_ntags));
            const int rc = pthread_create(&_workers[i], NULL, worker_thread, arg);
            if (rc) {
                delete arg;
                LOG(ERROR) << "Fail to create _workers[" << i << "], " << berror(rc);
                return -1;
            }
        }
        _worker_usage_second.expose("fiber_worker_usage");
        _switch_per_second.expose("fiber_switch_second");
        _signal_per_second.expose("fiber_signal_second");
        _status.expose("fiber_group_status");

        // Wait for at least one group is added so that choose_one_group()
        // never returns NULL.
        // TODO: Handle the case that worker quits before add_group
        for (int i = 0; i < turbo::get_flag(FLAGS_task_group_ntags);) {
            if (_tagged_ngroup[i].load(std::memory_order_acquire) == 0) {
                usleep(100);  // TODO: Elaborate
                continue;
            }
            ++i;
        }

        _init.store(true, std::memory_order_release);

        return 0;
    }

    int TaskControl::add_workers(int num, fiber_tag_t tag) {
        if (num <= 0) {
            return 0;
        }
        try {
            _workers.resize(_concurrency + num);
        } catch (...) {
            return 0;
        }
        const int old_concurency = _concurrency.load(std::memory_order_relaxed);
        for (int i = 0; i < num; ++i) {
            // Worker will add itself to _idle_workers, so we have to add
            // _concurrency before create a worker.
            _concurrency.fetch_add(1);
            auto arg = new WorkerThreadArgs(this, tag);
            const int rc = pthread_create(
                    &_workers[i + old_concurency], NULL, worker_thread, arg);
            if (rc) {
                delete arg;
                LOG(WARNING) << "Fail to create _workers[" << i + old_concurency
                             << "], " << berror(rc);
                _concurrency.fetch_sub(1, std::memory_order_release);
                break;
            }
        }
        // Cannot fail
        _workers.resize(_concurrency.load(std::memory_order_relaxed));
        return _concurrency.load(std::memory_order_relaxed) - old_concurency;
    }

    TaskGroup *TaskControl::choose_one_group(fiber_tag_t tag) {
        CHECK(tag >= FIBER_TAG_DEFAULT && tag < turbo::get_flag(FLAGS_task_group_ntags));
        auto &groups = tag_group(tag);
        const auto ngroup = tag_ngroup(tag).load(std::memory_order_acquire);
        if (ngroup != 0) {
            return groups[mutil::fast_rand_less_than(ngroup)];
        }
        CHECK(false) << "Impossible: ngroup is 0";
        return NULL;
    }

    extern int stop_and_join_epoll_threads();

    void TaskControl::stop_and_join() {
        // Close epoll threads so that worker threads are not waiting on epoll(
        // which cannot be woken up by signal_task below)
        CHECK_EQ(0, stop_and_join_epoll_threads());

        // Stop workers
        {
            MELON_SCOPED_LOCK(_modify_group_mutex);
            _stop = true;
            std::for_each(
                    _tagged_ngroup.begin(), _tagged_ngroup.end(),
                    [](std::atomic<size_t> &index) { index.store(0, std::memory_order_relaxed); });
        }
        for (int i = 0; i < turbo::get_flag(FLAGS_task_group_ntags); ++i) {
            for (auto &pl: _pl[i]) {
                pl.stop();
            }
        }
        // Interrupt blocking operations.
        for (size_t i = 0; i < _workers.size(); ++i) {
            interrupt_pthread(_workers[i]);
        }
        // Join workers
        for (size_t i = 0; i < _workers.size(); ++i) {
            pthread_join(_workers[i], NULL);
        }
    }

    TaskControl::~TaskControl() {
        // NOTE: g_task_control is not destructed now because the situation
        //       is extremely racy.
        delete _pending_time.exchange(NULL, std::memory_order_relaxed);
        _worker_usage_second.hide();
        _switch_per_second.hide();
        _signal_per_second.hide();
        _status.hide();

        stop_and_join();
    }

    int TaskControl::_add_group(TaskGroup *g, fiber_tag_t tag) {
        if (__builtin_expect(NULL == g, 0)) {
            return -1;
        }
        std::unique_lock<mutil::Mutex> mu(_modify_group_mutex);
        if (_stop) {
            return -1;
        }
        g->set_tag(tag);
        g->set_pl(&_pl[tag][mutil::fmix64(pthread_numeric_id()) % PARKING_LOT_NUM]);
        size_t ngroup = _tagged_ngroup[tag].load(std::memory_order_relaxed);
        if (ngroup < (size_t) FIBER_MAX_CONCURRENCY) {
            _tagged_groups[tag][ngroup] = g;
            _tagged_ngroup[tag].store(ngroup + 1, std::memory_order_release);
        }
        mu.unlock();
        // See the comments in _destroy_group
        // TODO: Not needed anymore since non-worker pthread cannot have TaskGroup
        // signal_task(65536, tag);
        return 0;
    }

    void TaskControl::delete_task_group(void *arg) {
        delete (TaskGroup *) arg;
    }

    int TaskControl::_destroy_group(TaskGroup *g) {
        if (NULL == g) {
            LOG(ERROR) << "Param[g] is NULL";
            return -1;
        }
        if (g->_control != this) {
            LOG(ERROR) << "TaskGroup=" << g
                       << " does not belong to this TaskControl=" << this;
            return -1;
        }
        bool erased = false;
        {
            MELON_SCOPED_LOCK(_modify_group_mutex);
            auto tag = g->tag();
            auto &groups = tag_group(tag);
            const size_t ngroup = tag_ngroup(tag).load(std::memory_order_relaxed);
            for (size_t i = 0; i < ngroup; ++i) {
                if (groups[i] == g) {
                    // No need for atomic_thread_fence because lock did it.
                    groups[i] = groups[ngroup - 1];
                    // Change _ngroup and keep _groups unchanged at last so that:
                    //  - If steal_task sees the newest _ngroup, it would not touch
                    //    _groups[ngroup -1]
                    //  - If steal_task sees old _ngroup and is still iterating on
                    //    _groups, it would not miss _groups[ngroup - 1] which was
                    //    swapped to _groups[i]. Although adding new group would
                    //    overwrite it, since we do signal_task in _add_group(),
                    //    we think the pending tasks of _groups[ngroup - 1] would
                    //    not miss.
                    tag_ngroup(tag).store(ngroup - 1, std::memory_order_release);
                    //_groups[ngroup - 1] = NULL;
                    erased = true;
                    break;
                }
            }
        }

        // Can't delete g immediately because for performance consideration,
        // we don't lock _modify_group_mutex in steal_task which may
        // access the removed group concurrently. We use simple strategy here:
        // Schedule a function which deletes the TaskGroup after
        // FLAGS_task_group_delete_delay seconds
        if (erased) {
            get_global_timer_thread()->schedule(
                    delete_task_group, g,
                    mutil::microseconds_from_now(turbo::get_flag(FLAGS_task_group_delete_delay) * 1000000L));
        }
        return 0;
    }

    bool TaskControl::steal_task(fiber_t *tid, size_t *seed, size_t offset) {
        auto tag = tls_task_group->tag();
        // 1: Acquiring fence is paired with releasing fence in _add_group to
        // avoid accessing uninitialized slot of _groups.
        const size_t ngroup = tag_ngroup(tag).load(std::memory_order_acquire/*1*/);
        if (0 == ngroup) {
            return false;
        }

        // NOTE: Don't return inside `for' iteration since we need to update |seed|
        bool stolen = false;
        size_t s = *seed;
        auto &groups = tag_group(tag);
        for (size_t i = 0; i < ngroup; ++i, s += offset) {
            TaskGroup *g = groups[s % ngroup];
            // g is possibly NULL because of concurrent _destroy_group
            if (g) {
                if (g->_rq.steal(tid)) {
                    stolen = true;
                    break;
                }
                if (g->_remote_rq.pop(tid)) {
                    stolen = true;
                    break;
                }
            }
        }
        *seed = s;
        return stolen;
    }

    void TaskControl::signal_task(int num_task, fiber_tag_t tag) {
        if (num_task <= 0) {
            return;
        }
        // TODO(gejun): Current algorithm does not guarantee enough threads will
        // be created to match caller's requests. But in another side, there's also
        // many useless signalings according to current impl. Capping the concurrency
        // is a good balance between performance and timeliness of scheduling.
        if (num_task > 2) {
            num_task = 2;
        }
        auto &pl = tag_pl(tag);
        int start_index = mutil::fmix64(pthread_numeric_id()) % PARKING_LOT_NUM;
        num_task -= pl[start_index].signal(1);
        if (num_task > 0) {
            for (int i = 1; i < PARKING_LOT_NUM && num_task > 0; ++i) {
                if (++start_index >= PARKING_LOT_NUM) {
                    start_index = 0;
                }
                num_task -= pl[start_index].signal(1);
            }
        }
        if (num_task > 0 &&
            turbo::get_flag(FLAGS_fiber_min_concurrency) > 0 &&    // test min_concurrency for performance
            _concurrency.load(std::memory_order_relaxed) < turbo::get_flag(FLAGS_fiber_concurrency)) {
            // TODO: Reduce this lock
            MELON_SCOPED_LOCK(g_task_control_mutex);
            if (_concurrency.load(std::memory_order_acquire) < turbo::get_flag(FLAGS_fiber_concurrency)) {
                add_workers(1, tag);
            }
        }
    }

    void TaskControl::print_rq_sizes(std::ostream &os) {
        size_t ngroup = 0;
        std::for_each(_tagged_ngroup.begin(), _tagged_ngroup.end(), [&](std::atomic<size_t> &index) {
            ngroup += index.load(std::memory_order_relaxed);
        });
        DEFINE_SMALL_ARRAY(int, nums, ngroup, 128);
        {
            MELON_SCOPED_LOCK(_modify_group_mutex);
            // ngroup > _ngroup: nums[_ngroup ... ngroup-1] = 0
            // ngroup < _ngroup: just ignore _groups[_ngroup ... ngroup-1]
            int i = 0;
            for_each_task_group([&](TaskGroup *g) {
                nums[i] = (g ? g->_rq.volatile_size() : 0);
                ++i;
            });
        }
        for (size_t i = 0; i < ngroup; ++i) {
            os << nums[i] << ' ';
        }
    }

    double TaskControl::get_cumulated_worker_time() {
        int64_t cputime_ns = 0;
        MELON_SCOPED_LOCK(_modify_group_mutex);
        for_each_task_group([&](TaskGroup *g) {
            if (g) {
                cputime_ns += g->_cumulated_cputime_ns;
            }
        });
        return cputime_ns / 1000000000.0;
    }

    double TaskControl::get_cumulated_worker_time_with_tag(fiber_tag_t tag) {
        int64_t cputime_ns = 0;
        MELON_SCOPED_LOCK(_modify_group_mutex);
        const size_t ngroup = tag_ngroup(tag).load(std::memory_order_relaxed);
        auto &groups = tag_group(tag);
        for (size_t i = 0; i < ngroup; ++i) {
            if (groups[i]) {
                cputime_ns += groups[i]->_cumulated_cputime_ns;
            }
        }
        return cputime_ns / 1000000000.0;
    }

    int64_t TaskControl::get_cumulated_switch_count() {
        int64_t c = 0;
        MELON_SCOPED_LOCK(_modify_group_mutex);
        for_each_task_group([&](TaskGroup *g) {
            if (g) {
                c += g->_nswitch;
            }
        });
        return c;
    }

    int64_t TaskControl::get_cumulated_signal_count() {
        int64_t c = 0;
        MELON_SCOPED_LOCK(_modify_group_mutex);
        for_each_task_group([&](TaskGroup *g) {
            if (g) {
                c += g->_nsignaled + g->_remote_nsignaled;
            }
        });
        return c;
    }

    melon::var::LatencyRecorder *TaskControl::create_exposed_pending_time() {
        bool is_creator = false;
        _pending_time_mutex.lock();
        melon::var::LatencyRecorder *pt = _pending_time.load(std::memory_order_consume);
        if (!pt) {
            pt = new melon::var::LatencyRecorder;
            _pending_time.store(pt, std::memory_order_release);
            is_creator = true;
        }
        _pending_time_mutex.unlock();
        if (is_creator) {
            pt->expose("fiber_creation");
        }
        return pt;
    }

}  // namespace fiber
