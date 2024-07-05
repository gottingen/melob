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


#include <melon/utility/compat.h>
#include <new>                                   // std::nothrow
#include <sys/poll.h>                            // poll()

#if defined(OS_MACOSX)
#include <sys/types.h>                           // struct kevent
#include <sys/event.h>                           // kevent(), kqueue()
#endif

#include <atomic>
#include <melon/utility/time.h>
#include <melon/utility/fd_utility.h>                     // make_non_blocking
#include <turbo/log/logging.h>
#include <melon/utility/third_party/murmurhash3/murmurhash3.h>   // fmix32
#include <melon/fiber/butex.h>                       // butex_*
#include <melon/fiber/task_group.h>                  // TaskGroup
#include <melon/fiber/fiber.h>                             // fiber_start_urgent

// Implement fiber functions on file descriptors

namespace fiber {

    extern MELON_THREAD_LOCAL TaskGroup *tls_task_group;

    template<typename T, size_t NBLOCK, size_t BLOCK_SIZE>
    class LazyArray {
        struct Block {
            std::atomic<T> items[BLOCK_SIZE];
        };

    public:
        LazyArray() {
            memset(static_cast<void *>(_blocks), 0, sizeof(std::atomic<Block *>) * NBLOCK);
        }

        std::atomic<T> *get_or_new(size_t index) {
            const size_t block_index = index / BLOCK_SIZE;
            if (block_index >= NBLOCK) {
                return NULL;
            }
            const size_t block_offset = index - block_index * BLOCK_SIZE;
            Block *b = _blocks[block_index].load(std::memory_order_consume);
            if (b != NULL) {
                return b->items + block_offset;
            }
            b = new(std::nothrow) Block;
            if (NULL == b) {
                b = _blocks[block_index].load(std::memory_order_consume);
                return (b ? b->items + block_offset : NULL);
            }
            // Set items to default value of T.
            std::fill(b->items, b->items + BLOCK_SIZE, T());
            Block *expected = NULL;
            if (_blocks[block_index].compare_exchange_strong(
                    expected, b, std::memory_order_release,
                    std::memory_order_consume)) {
                return b->items + block_offset;
            }
            delete b;
            return expected->items + block_offset;
        }

        std::atomic<T> *get(size_t index) const {
            const size_t block_index = index / BLOCK_SIZE;
            if (__builtin_expect(block_index < NBLOCK, 1)) {
                const size_t block_offset = index - block_index * BLOCK_SIZE;
                Block *const b = _blocks[block_index].load(std::memory_order_consume);
                if (__builtin_expect(b != NULL, 1)) {
                    return b->items + block_offset;
                }
            }
            return NULL;
        }

    private:
        std::atomic<Block *> _blocks[NBLOCK];
    };

    typedef std::atomic<int> EpollButex;

    static EpollButex *const CLOSING_GUARD = (EpollButex *) (intptr_t) -1L;

#ifndef NDEBUG
    std::atomic<int> break_nums = MUTIL_STATIC_ATOMIC_INIT(0);
#endif

// Able to address 67108864 file descriptors, should be enough.
    LazyArray<EpollButex *, 262144/*NBLOCK*/, 256/*BLOCK_SIZE*/> fd_butexes;

    static const int FIBER_DEFAULT_EPOLL_SIZE = 65536;

    class EpollThread {
    public:
        EpollThread()
                : _epfd(-1), _stop(false), _tid(0) {
        }

        int start(int epoll_size) {
            if (started()) {
                return -1;
            }
            _start_mutex.lock();
            // Double check
            if (started()) {
                _start_mutex.unlock();
                return -1;
            }
#if defined(OS_LINUX)
            _epfd = epoll_create(epoll_size);
#elif defined(OS_MACOSX)
            _epfd = kqueue();
#endif
            _start_mutex.unlock();
            if (_epfd < 0) {
                PLOG(FATAL) << "Fail to epoll_create/kqueue";
                return -1;
            }
            if (fiber_start_background(
                    &_tid, NULL, EpollThread::run_this, this) != 0) {
                close(_epfd);
                _epfd = -1;
                LOG(FATAL) << "Fail to create epoll fiber";
                return -1;
            }
            return 0;
        }

        // Note: This function does not wake up suspended fd_wait. This is fine
        // since stop_and_join is only called on program's termination
        // (g_task_control.stop()), suspended fibers do not block quit of
        // worker pthreads and completion of g_task_control.stop().
        int stop_and_join() {
            if (!started()) {
                return 0;
            }
            // No matter what this function returns, _epfd will be set to -1
            // (making started() false) to avoid latter stop_and_join() to
            // enter again.
            const int saved_epfd = _epfd;
            _epfd = -1;

            // epoll_wait cannot be woken up by closing _epfd. We wake up
            // epoll_wait by inserting a fd continuously triggering EPOLLOUT.
            // Visibility of _stop: constant EPOLLOUT forces epoll_wait to see
            // _stop (to be true) finally.
            _stop = true;
            int closing_epoll_pipe[2];
            if (pipe(closing_epoll_pipe)) {
                PLOG(FATAL) << "Fail to create closing_epoll_pipe";
                return -1;
            }
#if defined(OS_LINUX)
            epoll_event evt = {EPOLLOUT, {NULL}};
            if (epoll_ctl(saved_epfd, EPOLL_CTL_ADD,
                          closing_epoll_pipe[1], &evt) < 0) {
#elif defined(OS_MACOSX)
                struct kevent kqueue_event;
                EV_SET(&kqueue_event, closing_epoll_pipe[1], EVFILT_WRITE, EV_ADD | EV_ENABLE,
                        0, 0, NULL);
                if (kevent(saved_epfd, &kqueue_event, 1, NULL, 0, NULL) < 0) {
#endif
                PLOG(FATAL) << "Fail to add closing_epoll_pipe into epfd="
                             << saved_epfd;
                return -1;
            }

            const int rc = fiber_join(_tid, NULL);
            if (rc) {
                LOG(FATAL) << "Fail to join EpollThread, " << berror(rc);
                return -1;
            }
            close(closing_epoll_pipe[0]);
            close(closing_epoll_pipe[1]);
            close(saved_epfd);
            return 0;
        }

        int fd_wait(int fd, unsigned events, const timespec *abstime) {
            std::atomic<EpollButex *> *p = fd_butexes.get_or_new(fd);
            if (NULL == p) {
                errno = ENOMEM;
                return -1;
            }

            EpollButex *butex = p->load(std::memory_order_consume);
            if (NULL == butex) {
                // It is rare to wait on one file descriptor from multiple threads
                // simultaneously. Creating singleton by optimistic locking here
                // saves mutexes for each butex.
                butex = butex_create_checked<EpollButex>();
                butex->store(0, std::memory_order_relaxed);
                EpollButex *expected = NULL;
                if (!p->compare_exchange_strong(expected, butex,
                                                std::memory_order_release,
                                                std::memory_order_consume)) {
                    butex_destroy(butex);
                    butex = expected;
                }
            }

            while (butex == CLOSING_GUARD) {  // fiber_close() is running.
                if (sched_yield() < 0) {
                    return -1;
                }
                butex = p->load(std::memory_order_consume);
            }
            // Save value of butex before adding to epoll because the butex may
            // be changed before butex_wait. No memory fence because EPOLL_CTL_MOD
            // and EPOLL_CTL_ADD shall have release fence.
            const int expected_val = butex->load(std::memory_order_relaxed);

#if defined(OS_LINUX)
            epoll_event evt;
            evt.events = events;
            evt.data.fd = fd;
            if (epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &evt) < 0 &&
                errno != EEXIST) {
                PLOG(FATAL) << "Fail to add fd=" << fd << " into epfd=" << _epfd;
                return -1;
            }
#elif defined(OS_MACOSX)
            struct kevent kqueue_event;
            EV_SET(&kqueue_event, fd, events, EV_ADD | EV_ENABLE | EV_ONESHOT,
                    0, 0, butex);
            if (kevent(_epfd, &kqueue_event, 1, NULL, 0, NULL) < 0) {
                PLOG(FATAL) << "Fail to add fd=" << fd << " into kqueuefd=" << _epfd;
                return -1;
            }
#endif
            if (butex_wait(butex, expected_val, abstime) < 0 &&
                errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
            return 0;
        }

        int fd_close(int fd) {
            if (fd < 0) {
                // what close(-1) returns
                errno = EBADF;
                return -1;
            }
            std::atomic<EpollButex *> *pbutex = fiber::fd_butexes.get(fd);
            if (NULL == pbutex) {
                // Did not call fiber_fd functions, close directly.
                return close(fd);
            }
            EpollButex *butex = pbutex->exchange(
                    CLOSING_GUARD, std::memory_order_relaxed);
            if (butex == CLOSING_GUARD) {
                // concurrent double close detected.
                errno = EBADF;
                return -1;
            }
            if (butex != NULL) {
                butex->fetch_add(1, std::memory_order_relaxed);
                butex_wake_all(butex);
            }
#if defined(OS_LINUX)
            epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL);
#elif defined(OS_MACOSX)
            struct kevent evt;
            EV_SET(&evt, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            kevent(_epfd, &evt, 1, NULL, 0, NULL);
            EV_SET(&evt, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            kevent(_epfd, &evt, 1, NULL, 0, NULL);
#endif
            const int rc = close(fd);
            pbutex->exchange(butex, std::memory_order_relaxed);
            return rc;
        }

        bool started() const {
            return _epfd >= 0;
        }

    private:
        static void *run_this(void *arg) {
            return static_cast<EpollThread *>(arg)->run();
        }

        void *run() {
            const int initial_epfd = _epfd;
            const size_t MAX_EVENTS = 32;
#if defined(OS_LINUX)
            epoll_event *e = new(std::nothrow) epoll_event[MAX_EVENTS];
#elif defined(OS_MACOSX)
            typedef struct kevent KEVENT;
            struct kevent* e = new (std::nothrow) KEVENT[MAX_EVENTS];
#endif
            if (NULL == e) {
                LOG(FATAL) << "Fail to new epoll_event";
                return NULL;
            }

#if defined(OS_LINUX)
            DLOG(INFO) << "Use DEL+ADD instead of EPOLLONESHOT+MOD due to kernel bug. Performance will be much lower.";
#endif
            while (!_stop) {
                const int epfd = _epfd;
#if defined(OS_LINUX)
                const int n = epoll_wait(epfd, e, MAX_EVENTS, -1);
#elif defined(OS_MACOSX)
                const int n = kevent(epfd, NULL, 0, e, MAX_EVENTS, NULL);
#endif
                if (_stop) {
                    break;
                }

                if (n < 0) {
                    if (errno == EINTR) {
#ifndef NDEBUG
                        break_nums.fetch_add(1, std::memory_order_relaxed);
                        int* p = &errno;
                        const char* b = berror();
                        const char* b2 = berror(errno);
                        DLOG(FATAL) << "Fail to epoll epfd=" << epfd << ", "
                                    << errno << " " << p << " " <<  b << " " <<  b2;
#endif
                        continue;
                    }

                    PLOG(INFO) << "Fail to epoll epfd=" << epfd;
                    break;
                }

#if defined(OS_LINUX)
                for (int i = 0; i < n; ++i) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, e[i].data.fd, NULL);
                }
#endif
                for (int i = 0; i < n; ++i) {
#if defined(OS_LINUX)
                    std::atomic<EpollButex *> *pbutex = fd_butexes.get(e[i].data.fd);
                    EpollButex *butex = pbutex ?
                                        pbutex->load(std::memory_order_consume) : NULL;
#elif defined(OS_MACOSX)
                    EpollButex* butex = static_cast<EpollButex*>(e[i].udata);
#endif
                    if (butex != NULL && butex != CLOSING_GUARD) {
                        butex->fetch_add(1, std::memory_order_relaxed);
                        butex_wake_all(butex);
                    }
                }
            }

            delete[] e;
            DLOG(INFO) << "EpollThread=" << _tid << "(epfd="
                        << initial_epfd << ") is about to stop";
            return NULL;
        }

        int _epfd;
        bool _stop;
        fiber_t _tid;
        mutil::Mutex _start_mutex;
    };

    EpollThread epoll_thread[FIBER_EPOLL_THREAD_NUM];

    static inline EpollThread &get_epoll_thread(int fd) {
        if (FIBER_EPOLL_THREAD_NUM == 1UL) {
            EpollThread &et = epoll_thread[0];
            et.start(FIBER_DEFAULT_EPOLL_SIZE);
            return et;
        }

        EpollThread &et = epoll_thread[mutil::fmix32(fd) % FIBER_EPOLL_THREAD_NUM];
        et.start(FIBER_DEFAULT_EPOLL_SIZE);
        return et;
    }

//TODO(zhujiashun): change name
    int stop_and_join_epoll_threads() {
        // Returns -1 if any epoll thread failed to stop.
        int rc = 0;
        for (size_t i = 0; i < FIBER_EPOLL_THREAD_NUM; ++i) {
            if (epoll_thread[i].stop_and_join() < 0) {
                rc = -1;
            }
        }
        return rc;
    }

#if defined(OS_LINUX)

    short epoll_to_poll_events(uint32_t epoll_events) {
        // Most POLL* and EPOLL* are same values.
        uint32_t poll_events = (epoll_events &
                             (EPOLLIN | EPOLLPRI | EPOLLOUT |
                              EPOLLRDNORM | EPOLLRDBAND |
                              EPOLLWRNORM | EPOLLWRBAND |
                              EPOLLMSG | EPOLLERR | EPOLLHUP));
        DCHECK_EQ((uint32_t) poll_events, epoll_events);
        return poll_events;
    }

#elif defined(OS_MACOSX)
    static short kqueue_to_poll_events(int kqueue_events) {
        //TODO: add more values?
        short poll_events = 0;
        if (kqueue_events == EVFILT_READ) {
            poll_events |= POLLIN;
        }
        if (kqueue_events == EVFILT_WRITE) {
            poll_events |= POLLOUT;
        }
        return poll_events;
    }
#endif

// For pthreads.
    int pthread_fd_wait(int fd, unsigned events,
                        const timespec *abstime) {
        int diff_ms = -1;
        if (abstime) {
            timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            int64_t now_us = mutil::timespec_to_microseconds(now);
            int64_t abstime_us = mutil::timespec_to_microseconds(*abstime);
            if (abstime_us <= now_us) {
                errno = ETIMEDOUT;
                return -1;
            }
            diff_ms = (abstime_us - now_us + 999L) / 1000L;
        }
#if defined(OS_LINUX)
        const short poll_events = fiber::epoll_to_poll_events(events);
#elif defined(OS_MACOSX)
        const short poll_events = fiber::kqueue_to_poll_events(events);
#endif
        if (poll_events == 0) {
            errno = EINVAL;
            return -1;
        }
        pollfd ufds = {fd, poll_events, 0};
        const int rc = poll(&ufds, 1, diff_ms);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (ufds.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        return 0;
    }

}  // namespace fiber

extern "C" {

int fiber_fd_wait(int fd, unsigned events) {
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (NULL != g && !g->is_current_pthread_task()) {
        return fiber::get_epoll_thread(fd).fd_wait(
                fd, events, NULL);
    }
    return fiber::pthread_fd_wait(fd, events, NULL);
}

int fiber_fd_timedwait(int fd, unsigned events,
                       const timespec *abstime) {
    if (NULL == abstime) {
        return fiber_fd_wait(fd, events);
    }
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (NULL != g && !g->is_current_pthread_task()) {
        return fiber::get_epoll_thread(fd).fd_wait(
                fd, events, abstime);
    }
    return fiber::pthread_fd_wait(fd, events, abstime);
}

int fiber_connect(int sockfd, const sockaddr *serv_addr,
                  socklen_t addrlen) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (NULL == g || g->is_current_pthread_task()) {
        return ::connect(sockfd, serv_addr, addrlen);
    }
    // FIXME: Scoped non-blocking?
    mutil::make_non_blocking(sockfd);
    const int rc = connect(sockfd, serv_addr, addrlen);
    if (rc == 0 || errno != EINPROGRESS) {
        return rc;
    }
#if defined(OS_LINUX)
    if (fiber_fd_wait(sockfd, EPOLLOUT) < 0) {
#elif defined(OS_MACOSX)
        if (fiber_fd_wait(sockfd, EVFILT_WRITE) < 0) {
#endif
        return -1;
    }
    int err;
    socklen_t errlen = sizeof(err);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
        PLOG(FATAL) << "Fail to getsockopt";
        return -1;
    }
    if (err != 0) {
        CHECK(err != EINPROGRESS);
        errno = err;
        return -1;
    }
    return 0;
}

// This does not wake pthreads calling fiber_fd_*wait.
int fiber_close(int fd) {
    return fiber::get_epoll_thread(fd).fd_close(fd);
}

}  // extern "C"
