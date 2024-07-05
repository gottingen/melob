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


#ifndef MELON_FIBER_TASK_META_H_
#define MELON_FIBER_TASK_META_H_

#include <pthread.h>                 // pthread_spin_init
#include <melon/fiber/butex.h>           // butex_construct/destruct
#include <atomic>          // std::atomic
#include <melon/fiber/types.h>           // fiber_attr_t
#include <melon/fiber/stack.h>           // ContextualStack

namespace fiber {

struct TaskStatistics {
    int64_t cputime_ns;
    int64_t nswitch;
};

class KeyTable;
struct ButexWaiter;

struct LocalStorage {
    KeyTable* keytable;
    void* assigned_data;
    void* rpcz_parent_span;
};

#define FIBER_LOCAL_STORAGE_INITIALIZER { NULL, NULL, NULL }

const static LocalStorage LOCAL_STORAGE_INIT = FIBER_LOCAL_STORAGE_INITIALIZER;

struct TaskMeta {
    // [Not Reset]
    std::atomic<ButexWaiter*> current_waiter;
    uint64_t current_sleep;

    // A builtin flag to mark if the thread is stopping.
    bool stop;

    // The thread is interrupted and should wake up from some blocking ops.
    bool interrupted;

    // Scheduling of the thread can be delayed.
    bool about_to_quit;
    
    // [Not Reset] guarantee visibility of version_butex.
    pthread_spinlock_t version_lock;
    
    // [Not Reset] only modified by one fiber at any time, no need to be atomic
    uint32_t* version_butex;

    // The identifier. It does not have to be here, however many code is
    // simplified if they can get tid from TaskMeta.
    fiber_t tid;

    // User function and argument
    void* (*fn)(void*);
    void* arg;

    // Stack of this task.
    ContextualStack* stack;

    // Attributes creating this task
    fiber_attr_t attr;
    
    // Statistics
    int64_t cpuwide_start_ns;
    TaskStatistics stat;

    // fiber local storage, sync with tls_bls (defined in task_group.cpp)
    // when the fiber is created or destroyed.
    // DO NOT use this field directly, use tls_bls instead.
    LocalStorage local_storage;

public:
    // Only initialize [Not Reset] fields, other fields will be reset in
    // fiber_start* functions
    TaskMeta()
        : current_waiter(NULL)
        , current_sleep(0)
        , stack(NULL) {
        pthread_spin_init(&version_lock, 0);
        version_butex = butex_create_checked<uint32_t>();
        *version_butex = 1;
    }
        
    ~TaskMeta() {
        butex_destroy(version_butex);
        version_butex = NULL;
        pthread_spin_destroy(&version_lock);
    }

    void set_stack(ContextualStack* s) {
        stack = s;
    }

    ContextualStack* release_stack() {
        ContextualStack* tmp = stack;
        stack = NULL;
        return tmp;
    }

    StackType stack_type() const {
        return static_cast<StackType>(attr.stack_type);
    }
};

}  // namespace fiber

#endif  // MELON_FIBER_TASK_META_H_
