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
#include <melon/utility/macros.h>                         // MELON_CASSERT
#include <melon/fiber/butex.h>                       // butex_*
#include <melon/fiber/types.h>                       // fiber_cond_t

namespace fiber {
struct CondInternal {
    std::atomic<fiber_mutex_t*> m;
    std::atomic<int>* seq;
};

MELON_CASSERT(sizeof(CondInternal) == sizeof(fiber_cond_t),
              sizeof_innercond_must_equal_cond);
MELON_CASSERT(offsetof(CondInternal, m) == offsetof(fiber_cond_t, m),
              offsetof_cond_mutex_must_equal);
MELON_CASSERT(offsetof(CondInternal, seq) ==
              offsetof(fiber_cond_t, seq),
              offsetof_cond_seq_must_equal);
}

extern "C" {

extern int fiber_mutex_unlock(fiber_mutex_t*);
extern int fiber_mutex_lock_contended(fiber_mutex_t*);

int fiber_cond_init(fiber_cond_t* __restrict c,
                      const fiber_condattr_t*) {
    c->m = NULL;
    c->seq = fiber::butex_create_checked<int>();
    *c->seq = 0;
    return 0;
}

int fiber_cond_destroy(fiber_cond_t* c) {
    fiber::butex_destroy(c->seq);
    c->seq = NULL;
    return 0;
}

int fiber_cond_signal(fiber_cond_t* c) {
    fiber::CondInternal* ic = reinterpret_cast<fiber::CondInternal*>(c);
    // ic is probably dereferenced after fetch_add, save required fields before
    // this point
    std::atomic<int>* const saved_seq = ic->seq;
    saved_seq->fetch_add(1, std::memory_order_release);
    // don't touch ic any more
    fiber::butex_wake(saved_seq);
    return 0;
}

int fiber_cond_broadcast(fiber_cond_t* c) {
    fiber::CondInternal* ic = reinterpret_cast<fiber::CondInternal*>(c);
    fiber_mutex_t* m = ic->m.load(std::memory_order_relaxed);
    std::atomic<int>* const saved_seq = ic->seq;
    if (!m) {
        return 0;
    }
    void* const saved_butex = m->butex;
    // Wakeup one thread and requeue the rest on the mutex.
    ic->seq->fetch_add(1, std::memory_order_release);
    fiber::butex_requeue(saved_seq, saved_butex);
    return 0;
}

int fiber_cond_wait(fiber_cond_t* __restrict c,
                      fiber_mutex_t* __restrict m) {
    fiber::CondInternal* ic = reinterpret_cast<fiber::CondInternal*>(c);
    const int expected_seq = ic->seq->load(std::memory_order_relaxed);
    if (ic->m.load(std::memory_order_relaxed) != m) {
        // bind m to c
        fiber_mutex_t* expected_m = NULL;
        if (!ic->m.compare_exchange_strong(
                expected_m, m, std::memory_order_relaxed)) {
            return EINVAL;
        }
    }
    fiber_mutex_unlock(m);
    int rc1 = 0;
    if (fiber::butex_wait(ic->seq, expected_seq, NULL) < 0 &&
        errno != EWOULDBLOCK && errno != EINTR/*note*/) {
        // EINTR should not be returned by cond_*wait according to docs on
        // pthread, however spurious wake-up is OK, just as we do here
        // so that users can check flags in the loop often companioning
        // with the cond_wait ASAP. For example:
        //   mutex.lock();
        //   while (!stop && other-predicates) {
        //     cond_wait(&mutex);
        //   }
        //   mutex.unlock();
        // After interruption, above code should wake up from the cond_wait
        // soon and check the `stop' flag and other predicates.
        rc1 = errno;
    }
    const int rc2 = fiber_mutex_lock_contended(m);
    return (rc2 ? rc2 : rc1);
}

int fiber_cond_timedwait(fiber_cond_t* __restrict c,
                           fiber_mutex_t* __restrict m,
                           const struct timespec* __restrict abstime) {
    fiber::CondInternal* ic = reinterpret_cast<fiber::CondInternal*>(c);
    const int expected_seq = ic->seq->load(std::memory_order_relaxed);
    if (ic->m.load(std::memory_order_relaxed) != m) {
        // bind m to c
        fiber_mutex_t* expected_m = NULL;
        if (!ic->m.compare_exchange_strong(
                expected_m, m, std::memory_order_relaxed)) {
            return EINVAL;
        }
    }
    fiber_mutex_unlock(m);
    int rc1 = 0;
    if (fiber::butex_wait(ic->seq, expected_seq, abstime) < 0 &&
        errno != EWOULDBLOCK && errno != EINTR/*note*/) {
        // note: see comments in fiber_cond_wait on EINTR.
        rc1 = errno;
    }
    const int rc2 = fiber_mutex_lock_contended(m);
    return (rc2 ? rc2 : rc1);
}

}  // extern "C"
