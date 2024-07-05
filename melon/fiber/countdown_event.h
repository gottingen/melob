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

#include <melon/fiber/fiber.h>

namespace fiber {

    // A synchronization primitive to wait for multiple signallers.
    class CountdownEvent {
    public:
        CountdownEvent(int initial_count = 1);

        ~CountdownEvent();

        // Increase current counter by |v|
        void add_count(int v = 1);

        // Reset the counter to |v|
        void reset(int v = 1);

        // Decrease the counter by |sig|
        // when flush is true, after signal we need to call fiber_flush
        void signal(int sig = 1, bool flush = false);

        // Block current thread until the counter reaches 0.
        // Returns 0 on success, error code otherwise.
        // This method never returns EINTR.
        int wait();

        // Block the current thread until the counter reaches 0 or duetime has expired
        // Returns 0 on success, error code otherwise. ETIMEDOUT is for timeout.
        // This method never returns EINTR.
        int timed_wait(const timespec &duetime);

    private:
        int *_butex;
        bool _wait_was_invoked;
    };

}  // namespace fiber
