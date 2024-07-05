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

#include <atomic>
#include <melon/fiber/sys_futex.h>

namespace fiber {

// Park idle workers.
    class MELON_CACHELINE_ALIGNMENT ParkingLot {
    public:
        class State {
        public:
            State() : val(0) {}

            bool stopped() const { return val & 1; }

        private:
            friend class ParkingLot;

            State(int val) : val(val) {}

            int val;
        };

        ParkingLot() : _pending_signal(0) {}

        // Wake up at most `num_task' workers.
        // Returns #workers woken up.
        int signal(int num_task) {
            _pending_signal.fetch_add((num_task << 1), std::memory_order_release);
            return futex_wake_private(&_pending_signal, num_task);
        }

        // Get a state for later wait().
        State get_state() {
            return _pending_signal.load(std::memory_order_acquire);
        }

        // Wait for tasks.
        // If the `expected_state' does not match, wait() may finish directly.
        void wait(const State &expected_state) {
            futex_wait_private(&_pending_signal, expected_state.val, NULL);
        }

        // Wakeup suspended wait() and make them unwaitable ever.
        void stop() {
            _pending_signal.fetch_or(1);
            futex_wake_private(&_pending_signal, 10000);
        }

    private:
        // higher 31 bits for signalling, LSB for stopping.
        std::atomic<int> _pending_signal;
    };

}  // namespace fiber
