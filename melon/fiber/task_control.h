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


#ifndef MELON_FIBER_TASK_CONTROL_H_
#define MELON_FIBER_TASK_CONTROL_H_

#ifndef NDEBUG

#include <iostream>                             // std::ostream

#endif

#include <stddef.h>                             // size_t
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <melon/var/var.h>                          // melon::var::PassiveStatus
#include <melon/fiber/task_meta.h>                  // TaskMeta
#include <melon/utility/resource_pool.h>                 // ResourcePool
#include <melon/fiber/work_stealing_queue.h>        // WorkStealingQueue
#include <melon/fiber/parking_lot.h>

namespace fiber {

    class TaskGroup;

    // Control all task groups
    class TaskControl {
        friend class TaskGroup;

    public:
        TaskControl();

        ~TaskControl();

        // Must be called before using. `nconcurrency' is # of worker pthreads.
        int init(int nconcurrency);

        // Create a TaskGroup in this control.
        TaskGroup *create_group(fiber_tag_t tag);

        // Steal a task from a "random" group.
        bool steal_task(fiber_t *tid, size_t *seed, size_t offset);

        // Tell other groups that `n' tasks was just added to caller's runqueue
        void signal_task(int num_task, fiber_tag_t tag);

        // Stop and join worker threads in TaskControl.
        void stop_and_join();

        // Get # of worker threads.
        int concurrency() const { return _concurrency.load(std::memory_order_acquire); }

        int concurrency(fiber_tag_t tag) const { return _tagged_ngroup[tag].load(std::memory_order_acquire); }

        void print_rq_sizes(std::ostream &os);

        double get_cumulated_worker_time();

        double get_cumulated_worker_time_with_tag(fiber_tag_t tag);

        int64_t get_cumulated_switch_count();

        int64_t get_cumulated_signal_count();

        // [Not thread safe] Add more worker threads.
        // Return the number of workers actually added, which may be less than |num|
        int add_workers(int num, fiber_tag_t tag);

        // Choose one TaskGroup (randomly right now).
        // If this method is called after init(), it never returns NULL.
        TaskGroup *choose_one_group(fiber_tag_t tag = FIBER_TAG_DEFAULT);

    private:
        typedef std::array<TaskGroup *, FIBER_MAX_CONCURRENCY> TaggedGroups;
        static const int PARKING_LOT_NUM = 4;
        typedef std::array<ParkingLot, PARKING_LOT_NUM> TaggedParkingLot;

        // Add/Remove a TaskGroup.
        // Returns 0 on success, -1 otherwise.
        int _add_group(TaskGroup *, fiber_tag_t tag);

        int _destroy_group(TaskGroup *);

        // Tag group
        TaggedGroups &tag_group(fiber_tag_t tag) { return _tagged_groups[tag]; }

        // Tag ngroup
        std::atomic<size_t> &tag_ngroup(int tag) { return _tagged_ngroup[tag]; }

        // Tag parking slot
        TaggedParkingLot &tag_pl(fiber_tag_t tag) { return _pl[tag]; }

        static void delete_task_group(void *arg);

        static void *worker_thread(void *task_control);

        template<typename F>
        void for_each_task_group(F const &f);

        melon::var::LatencyRecorder &exposed_pending_time();

        melon::var::LatencyRecorder *create_exposed_pending_time();

        melon::var::Adder<int64_t> &tag_nworkers(fiber_tag_t tag);

        melon::var::Adder<int64_t> &tag_nfibers(fiber_tag_t tag);

        std::vector<std::atomic<size_t>> _tagged_ngroup;
        std::vector<TaggedGroups> _tagged_groups;
        mutil::Mutex _modify_group_mutex;

        std::atomic<bool> _init;  // if not init, var will case coredump
        bool _stop;
        std::atomic<int> _concurrency;
        std::vector<pthread_t> _workers;
        std::atomic<int> _next_worker_id;

        melon::var::Adder<int64_t> _nworkers;
        mutil::Mutex _pending_time_mutex;
        std::atomic<melon::var::LatencyRecorder *> _pending_time;
        melon::var::PassiveStatus<double> _cumulated_worker_time;
        melon::var::PerSecond<melon::var::PassiveStatus<double> > _worker_usage_second;
        melon::var::PassiveStatus<int64_t> _cumulated_switch_count;
        melon::var::PerSecond<melon::var::PassiveStatus<int64_t> > _switch_per_second;
        melon::var::PassiveStatus<int64_t> _cumulated_signal_count;
        melon::var::PerSecond<melon::var::PassiveStatus<int64_t> > _signal_per_second;
        melon::var::PassiveStatus<std::string> _status;
        melon::var::Adder<int64_t> _nfibers;

        std::vector<melon::var::Adder<int64_t> *> _tagged_nworkers;
        std::vector<melon::var::PassiveStatus<double> *> _tagged_cumulated_worker_time;
        std::vector<melon::var::PerSecond<melon::var::PassiveStatus<double>> *> _tagged_worker_usage_second;
        std::vector<melon::var::Adder<int64_t> *> _tagged_nfibers;

        std::vector<TaggedParkingLot> _pl;
    };

    inline melon::var::LatencyRecorder &TaskControl::exposed_pending_time() {
        melon::var::LatencyRecorder *pt = _pending_time.load(std::memory_order_consume);
        if (!pt) {
            pt = create_exposed_pending_time();
        }
        return *pt;
    }

    inline melon::var::Adder<int64_t> &TaskControl::tag_nworkers(fiber_tag_t tag) {
        return *_tagged_nworkers[tag];
    }

    inline melon::var::Adder<int64_t> &TaskControl::tag_nfibers(fiber_tag_t tag) {
        return *_tagged_nfibers[tag];
    }

    template<typename F>
    inline void TaskControl::for_each_task_group(F const &f) {
        if (_init.load(std::memory_order_acquire) == false) {
            return;
        }
        for (size_t i = 0; i < _tagged_groups.size(); ++i) {
            auto ngroup = tag_ngroup(i).load(std::memory_order_relaxed);
            auto &groups = tag_group(i);
            for (size_t j = 0; j < ngroup; ++j) {
                f(groups[j]);
            }
        }
    }

}  // namespace fiber

#endif  // MELON_FIBER_TASK_CONTROL_H_
