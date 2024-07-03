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

#pragma once

#include <turbo/log/logging.h>
#include <melon/fiber/fiber.h>

namespace melon {

    class FiberCond {
    public:
        FiberCond(int count = 0) {
            fiber_cond_init(&_cond, nullptr);
            fiber_mutex_init(&_mutex, nullptr);
            _count = count;
        }

        ~FiberCond() {
            fiber_mutex_destroy(&_mutex);
            fiber_cond_destroy(&_cond);
        }

        int count() const {
            return _count;
        }

        void increase() {
            fiber_mutex_lock(&_mutex);
            ++_count;
            fiber_mutex_unlock(&_mutex);
        }

        void decrease_signal() {
            fiber_mutex_lock(&_mutex);
            --_count;
            fiber_cond_signal(&_cond);
            fiber_mutex_unlock(&_mutex);
        }

        void decrease_broadcast() {
            fiber_mutex_lock(&_mutex);
            --_count;
            fiber_cond_broadcast(&_cond);
            fiber_mutex_unlock(&_mutex);
        }

        int wait(int cond = 0) {
            int ret = 0;
            fiber_mutex_lock(&_mutex);
            while (_count > cond) {
                ret = fiber_cond_wait(&_cond, &_mutex);
                if (ret != 0) {
                    LOG(WARNING) << "wait timeout, ret:" << ret;
                    break;
                }
            }
            fiber_mutex_unlock(&_mutex);
            return ret;
        }

        int increase_wait(int cond = 0) {
            int ret = 0;
            fiber_mutex_lock(&_mutex);
            while (_count + 1 > cond) {
                ret = fiber_cond_wait(&_cond, &_mutex);
                if (ret != 0) {
                    LOG(WARNING) << "wait timeout, ret:" << ret;
                    break;
                }
            }
            ++_count; // 不能放在while前面
            fiber_mutex_unlock(&_mutex);
            return ret;
        }

        int timed_wait(int64_t timeout_us, int cond = 0) {
            int ret = 0;
            timespec tm = mutil::microseconds_from_now(timeout_us);
            fiber_mutex_lock(&_mutex);
            while (_count > cond) {
                ret = fiber_cond_timedwait(&_cond, &_mutex, &tm);
                if (ret != 0) {
                    LOG(WARNING) << "wait timeout, ret:" << ret;
                    break;
                }
            }
            fiber_mutex_unlock(&_mutex);
            return ret;
        }

        int increase_timed_wait(int64_t timeout_us, int cond = 0) {
            int ret = 0;
            timespec tm = mutil::microseconds_from_now(timeout_us);
            fiber_mutex_lock(&_mutex);
            while (_count + 1 > cond) {
                ret = fiber_cond_timedwait(&_cond, &_mutex, &tm);
                if (ret != 0) {
                    LOG(WARNING) << "wait timeout, ret:" << ret;
                    break;
                }
            }
            ++_count;
            fiber_mutex_unlock(&_mutex);
            return ret;
        }

    private:
        int _count;
        fiber_cond_t _cond;
        fiber_mutex_t _mutex;
    };

    class Fiber {
    public:
        Fiber() {
        }

        explicit Fiber(const fiber_attr_t *attr) : _attr(attr) {
        }

        void run(const std::function<void()> &call) {
            std::function<void()> *_call = new std::function<void()>;
            *_call = call;
            int ret = fiber_start_background(&_tid, _attr,
                                               [](void *p) -> void * {
                                                   auto call = static_cast<std::function<void()> *>(p);
                                                   (*call)();
                                                   delete call;
                                                   return nullptr;
                                               }, _call);
            if (ret != 0) {
                LOG(ERROR)<< "fiber_start_background fail";
            }
        }

        void run_urgent(const std::function<void()> &call) {
            std::function<void()> *_call = new std::function<void()>;
            *_call = call;
            int ret = fiber_start_urgent(&_tid, _attr,
                                           [](void *p) -> void * {
                                               auto call = static_cast<std::function<void()> *>(p);
                                               (*call)();
                                               delete call;
                                               return nullptr;
                                           }, _call);
            if (ret != 0) {
                LOG(ERROR)<< "fiber_start_urgent fail";
            }
        }

        void join() {
            fiber_join(_tid, nullptr);
        }

        fiber_t id() {
            return _tid;
        }

    private:
        fiber_t _tid;
        const fiber_attr_t *_attr = nullptr;
    };

    class ConcurrencyFiber {
    public:
        explicit ConcurrencyFiber(int concurrency) :
                _concurrency(concurrency) {
        }

        ConcurrencyFiber(int concurrency, const fiber_attr_t *attr) :
                _concurrency(concurrency),
                _attr(attr) {
        }

        void run(const std::function<void()> &call) {
            _cond.increase_wait(_concurrency);
            Fiber bth(_attr);
            bth.run([this, call]() {
                call();
                _cond.decrease_signal();
            });
        }

        void join() {
            _cond.wait();
        }

        int count() const {
            return _cond.count();
        }

    private:
        int _concurrency = 10;
        FiberCond _cond;
        const fiber_attr_t *_attr = nullptr;
    };

}  // namespace melon
