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
#include <melon/fiber/butex.h>
#include <melon/fiber/countdown_event.h>

namespace fiber {

    CountdownEvent::CountdownEvent(int initial_count) {
        RELEASE_ASSERT_VERBOSE(initial_count >= 0,
                               "Invalid initial_count=%d",
                               initial_count);
        _butex = butex_create_checked<int>();
        *_butex = initial_count;
        _wait_was_invoked = false;
    }

    CountdownEvent::~CountdownEvent() {
        butex_destroy(_butex);
    }

    void CountdownEvent::signal(int sig, bool flush) {
        // Have to save _butex, *this is probably defreferenced by the wait thread
        // which sees fetch_sub
        void *const saved_butex = _butex;
        const int prev = ((std::atomic<int> *) _butex)
                ->fetch_sub(sig, std::memory_order_release);
        // DON'T touch *this ever after
        if (prev > sig) {
            return;
        }
        LOG_IF(ERROR, prev < sig) << "Counter is over decreased";
        butex_wake_all(saved_butex, flush);
    }

    int CountdownEvent::wait() {
        _wait_was_invoked = true;
        for (;;) {
            const int seen_counter =
                    ((std::atomic<int> *) _butex)->load(std::memory_order_acquire);
            if (seen_counter <= 0) {
                return 0;
            }
            if (butex_wait(_butex, seen_counter, NULL) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return errno;
            }
        }
    }

    void CountdownEvent::add_count(int v) {
        if (v <= 0) {
            LOG_IF(ERROR, v < 0) << "Invalid count=" << v;
            return;
        }
        LOG_IF(ERROR, _wait_was_invoked)
        << "Invoking add_count() after wait() was invoked";
        ((std::atomic<int> *) _butex)->fetch_add(v, std::memory_order_release);
    }

    void CountdownEvent::reset(int v) {
        if (v < 0) {
            LOG(ERROR) << "Invalid count=" << v;
            return;
        }
        const int prev_counter =
                ((std::atomic<int> *) _butex)
                        ->exchange(v, std::memory_order_release);
        LOG_IF(ERROR, _wait_was_invoked && prev_counter)
        << "Invoking reset() while count=" << prev_counter;
        _wait_was_invoked = false;
    }

    int CountdownEvent::timed_wait(const timespec &duetime) {
        _wait_was_invoked = true;
        for (;;) {
            const int seen_counter =
                    ((std::atomic<int> *) _butex)->load(std::memory_order_acquire);
            if (seen_counter <= 0) {
                return 0;
            }
            if (butex_wait(_butex, seen_counter, &duetime) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return errno;
            }
        }
    }

}  // namespace fiber
