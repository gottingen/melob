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

#include <melon/utility/macros.h>
#include <atomic>
#include <turbo/log/logging.h>

namespace fiber {

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue()
        : _bottom(1)
        , _capacity(0)
        , _buffer(nullptr)
        , _top(1) {
    }

    ~WorkStealingQueue() {
        delete [] _buffer;
        _buffer = nullptr;
    }

    int init(size_t capacity) {
        if (_capacity != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (capacity == 0) {
            LOG(ERROR) << "Invalid capacity=" << capacity;
            return -1;
        }
        if (capacity & (capacity - 1)) {
            LOG(ERROR) << "Invalid capacity=" << capacity
                       << " which must be power of 2";
            return -1;
        }
        _buffer = new(std::nothrow) T[capacity];
        if (nullptr == _buffer) {
            return -1;
        }
        _capacity = capacity;
        return 0;
    }

    // Push an item into the queue.
    // Returns true on pushed.
    // May run in parallel with steal().
    // Never run in parallel with pop() or another push().
    bool push(const T& x) {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        const size_t t = _top.load(std::memory_order_acquire);
        if (b >= t + _capacity) { // Full queue.
            return false;
        }
        _buffer[b & (_capacity - 1)] = x;
        _bottom.store(b + 1, std::memory_order_release);
        return true;
    }

    // Pop an item from the queue.
    // Returns true on popped and the item is written to `val'.
    // May run in parallel with steal().
    // Never run in parallel with push() or another pop().
    bool pop(T* val) {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        size_t t = _top.load(std::memory_order_relaxed);
        if (t >= b) {
            // fast check since we call pop() in each sched.
            // Stale _top which is smaller should not enter this branch.
            return false;
        }
        const size_t newb = b - 1;
        _bottom.store(newb, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        t = _top.load(std::memory_order_relaxed);
        if (t > newb) {
            _bottom.store(b, std::memory_order_relaxed);
            return false;
        }
        *val = _buffer[newb & (_capacity - 1)];
        if (t != newb) {
            return true;
        }
        // Single last element, compete with steal()
        const bool popped = _top.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        _bottom.store(b, std::memory_order_relaxed);
        return popped;
    }

    // Steal one item from the queue.
    // Returns true on stolen.
    // May run in parallel with push() pop() or another steal().
    bool steal(T* val) {
        size_t t = _top.load(std::memory_order_acquire);
        size_t b = _bottom.load(std::memory_order_acquire);
        if (t >= b) {
            // Permit false negative for performance considerations.
            return false;
        }
        do {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            b = _bottom.load(std::memory_order_acquire);
            if (t >= b) {
                return false;
            }
            *val = _buffer[t & (_capacity - 1)];
        } while (!_top.compare_exchange_strong(t, t + 1,
                                               std::memory_order_seq_cst,
                                               std::memory_order_relaxed));
        return true;
    }

    size_t volatile_size() const {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        const size_t t = _top.load(std::memory_order_relaxed);
        return (b <= t ? 0 : (b - t));
    }

    size_t capacity() const { return _capacity; }

private:
    // Copying a concurrent structure makes no sense.
    DISALLOW_COPY_AND_ASSIGN(WorkStealingQueue);

    std::atomic<size_t> _bottom;
    size_t _capacity;
    T* _buffer;
    MELON_CACHELINE_ALIGNMENT std::atomic<size_t> _top;
};

}  // namespace fiber
