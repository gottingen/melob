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


#include <turbo/flags/flag.h>
#include <melon/utility/macros.h>                       // MELON_CASSERT
#include <turbo/log/logging.h>
#include <melon/fiber/task_group.h>                // TaskGroup
#include <melon/fiber/task_control.h>              // TaskControl
#include <melon/fiber/timer_thread.h>
#include <melon/fiber/list_of_abafree_id.h>
#include <melon/fiber/fiber.h>
#include <turbo/strings/str_format.h>

namespace fiber {
    static bool never_set_fiber_concurrency = true;
    static bool never_set_fiber_concurrency_by_tag = true;

    static void validate_fiber_min_concurrency() noexcept;
    static bool validate_fiber_current_tag(std::string_view value, std::string *err) noexcept;
    static bool validate_fiber_concurrency_by_tag(std::string_view value, std::string *err) noexcept;
}  // namespace fiber
TURBO_FLAG(int32_t, fiber_concurrency, 8 + FIBER_EPOLL_THREAD_NUM,
           "Number of pthread workers")
           .on_validate(turbo::GeValidator<int32_t, 8 + FIBER_EPOLL_THREAD_NUM>::validate)
           .on_update([]() noexcept {
               auto num = turbo::get_flag(FLAGS_fiber_concurrency);
               auto r = fiber_setconcurrency(num);
                if (r != 0) {
                     LOG(ERROR) << "Fail to set fiber_concurrency=" << num;
                }
           });

TURBO_FLAG(int32_t, fiber_min_concurrency, 0,
           "Initial number of pthread workers which will be added on-demand."
           " The laziness is disabled when this value is non-positive,"
           " and workers will be created eagerly according to -fiber_concurrency and fiber_setconcurrency(). ")
           .on_validate(turbo::GeValidator<int32_t, 0>::validate)
           .on_update(fiber::validate_fiber_min_concurrency);


TURBO_FLAG(int32_t, fiber_current_tag, FIBER_TAG_DEFAULT, "Set fiber concurrency for this tag")
           .on_validate(fiber::validate_fiber_current_tag);

TURBO_FLAG(int32_t, fiber_concurrency_by_tag, 0,
           "Number of pthread workers of FLAGS_fiber_current_tag").on_validate(fiber::validate_fiber_concurrency_by_tag);

namespace fiber {

    static_assert(sizeof(TaskControl *) == sizeof(std::atomic<TaskControl *>), "atomic size match");

    pthread_mutex_t g_task_control_mutex = PTHREAD_MUTEX_INITIALIZER;
    // Referenced in rpc, needs to be extern.
    // Notice that we can't declare the variable as atomic<TaskControl*> which
    // are not constructed before main().
    TaskControl *g_task_control = nullptr;

    extern MELON_THREAD_LOCAL TaskGroup *tls_task_group;

    extern void (*g_worker_startfn)();

    extern void (*g_tagged_worker_startfn)(fiber_tag_t);

    inline TaskControl *get_task_control() {
        return g_task_control;
    }

    inline TaskControl *get_or_new_task_control() {
        std::atomic<TaskControl *> *p = (std::atomic<TaskControl *> *) &g_task_control;
        TaskControl *c = p->load(std::memory_order_consume);
        if (c != nullptr) {
            return c;
        }
        MELON_SCOPED_LOCK(g_task_control_mutex);
        c = p->load(std::memory_order_consume);
        if (c != nullptr) {
            return c;
        }
        c = new(std::nothrow) TaskControl;
        if (nullptr == c) {
            return nullptr;
        }
        int concurrency = turbo::get_flag(FLAGS_fiber_min_concurrency) > 0 ?
                          turbo::get_flag(FLAGS_fiber_min_concurrency) :
                          turbo::get_flag(FLAGS_fiber_concurrency);
        if (c->init(concurrency) != 0) {
            LOG(ERROR) << "Fail to init g_task_control";
            delete c;
            return nullptr;
        }
        p->store(c, std::memory_order_release);
        return c;
    }

    static int add_workers_for_each_tag(int num) {
        int added = 0;
        auto c = get_task_control();
        for (auto i = 0; i < num; ++i) {
            added += c->add_workers(1, i % turbo::get_flag(FLAGS_task_group_ntags));
        }
        return added;
    }

    void validate_fiber_min_concurrency() noexcept {
        int val = turbo::get_flag(FLAGS_fiber_min_concurrency);
        if (val <= 0) {
            return;
        }
        if (val < FIBER_MIN_CONCURRENCY || val > turbo::get_flag(FLAGS_fiber_concurrency)) {
            return;
        }
        TaskControl *c = get_task_control();
        if (!c) {
            return;
        }
        MELON_SCOPED_LOCK(g_task_control_mutex);
        int concurrency = c->concurrency();
        if (val > concurrency) {
            int added = fiber::add_workers_for_each_tag(val - concurrency);
            auto r = added == (val - concurrency);
            if (!r) {
                LOG(ERROR)<<"Fail to add workers for min_concurrency="<<val;
            }
        }
    }

    static bool validate_fiber_current_tag(std::string_view value, std::string *err) noexcept {
        int val;
        if (!::turbo::parse_flag(value, &val, err)) {
            return false;
        }
        if (val < FIBER_TAG_DEFAULT || val >= turbo::get_flag(FLAGS_task_group_ntags)) {
            if(err) {
                *err = turbo::str_format("Invalid fiber_current_tag=%s", value);
            }
            return false;
        }
        MELON_SCOPED_LOCK(fiber::g_task_control_mutex);
        auto c = fiber::get_task_control();
        if (c == nullptr) {
            turbo::set_flag(&FLAGS_fiber_concurrency_by_tag, 0);
            return true;
        }
        turbo::set_flag(&FLAGS_fiber_concurrency_by_tag, c->concurrency(val));
        return true;
    }

    static bool validate_fiber_concurrency_by_tag(std::string_view value, std::string *err) noexcept {
        int val;
        if (!::turbo::parse_flag(value, &val, err)) {
            return false;
        }
        auto r = fiber_setconcurrency_by_tag(val, turbo::get_flag(FLAGS_fiber_current_tag)) == 0;
        if (!r && err) {
            *err = turbo::str_format("Invalid concurrency_by_tag=%s", value);
        }
        return r;
    }

    __thread TaskGroup *tls_task_group_nosignal = nullptr;

    MUTIL_FORCE_INLINE int
    start_from_non_worker(fiber_t *__restrict tid,
                          const fiber_attr_t *__restrict attr,
                          void *(*fn)(void *),
                          void *__restrict arg) {
        TaskControl *c = get_or_new_task_control();
        if (nullptr == c) {
            return ENOMEM;
        }
        auto tag = FIBER_TAG_DEFAULT;
        if (attr != nullptr && attr->tag != FIBER_TAG_INVALID) {
            tag = attr->tag;
        }
        if (attr != nullptr && (attr->flags & FIBER_NOSIGNAL)) {
            // Remember the TaskGroup to insert NOSIGNAL tasks for 2 reasons:
            // 1. NOSIGNAL is often for creating many fibers in batch,
            //    inserting into the same TaskGroup maximizes the batch.
            // 2. fiber_flush() needs to know which TaskGroup to flush.
            auto g = tls_task_group_nosignal;
            if (nullptr == g) {
                g = c->choose_one_group(tag);
                tls_task_group_nosignal = g;
            }
            return g->start_background<true>(tid, attr, fn, arg);
        }
        return c->choose_one_group(tag)->start_background<true>(tid, attr, fn, arg);
    }

    // Meet one of the three conditions, can run in thread local
    // attr is nullptr
    // tag equal to thread local
    // tag equal to FIBER_TAG_INVALID
    MUTIL_FORCE_INLINE bool can_run_thread_local(const fiber_attr_t *__restrict attr) {
        return attr == nullptr || attr->tag == fiber::tls_task_group->tag() ||
               attr->tag == FIBER_TAG_INVALID;
    }

    struct TidTraits {
        static const size_t BLOCK_SIZE = 63;
        static const size_t MAX_ENTRIES = 65536;
        static const fiber_t ID_INIT;

        static bool exists(fiber_t id) { return fiber::TaskGroup::exists(id); }
    };

    const fiber_t TidTraits::ID_INIT = INVALID_FIBER;

    typedef ListOfABAFreeId<fiber_t, TidTraits> TidList;

    struct TidStopper {
        void operator()(fiber_t id) const { fiber_stop(id); }
    };

    struct TidJoiner {
        void operator()(fiber_t &id) const {
            fiber_join(id, nullptr);
            id = INVALID_FIBER;
        }
    };

}  // namespace fiber

extern "C" {

int fiber_start_urgent(fiber_t *__restrict tid,
                       const fiber_attr_t *__restrict attr,
                       void *(*fn)(void *),
                       void *__restrict arg) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (g) {
        // if attribute is null use thread local task group
        if (fiber::can_run_thread_local(attr)) {
            return fiber::TaskGroup::start_foreground(&g, tid, attr, fn, arg);
        }
    }
    return fiber::start_from_non_worker(tid, attr, fn, arg);
}

int fiber_start_background(fiber_t *__restrict tid,
                           const fiber_attr_t *__restrict attr,
                           void *(*fn)(void *),
                           void *__restrict arg) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (g) {
        // if attribute is null use thread local task group
        if (fiber::can_run_thread_local(attr)) {
            return g->start_background<false>(tid, attr, fn, arg);
        }
    }
    return fiber::start_from_non_worker(tid, attr, fn, arg);
}

void fiber_flush() {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (g) {
        return g->flush_nosignal_tasks();
    }
    g = fiber::tls_task_group_nosignal;
    if (g) {
        // NOSIGNAL tasks were created in this non-worker.
        fiber::tls_task_group_nosignal = nullptr;
        return g->flush_nosignal_tasks_remote();
    }
}

int fiber_interrupt(fiber_t tid) {
    return fiber::TaskGroup::interrupt(tid, fiber::get_task_control());
}

int fiber_stop(fiber_t tid) {
    fiber::TaskGroup::set_stopped(tid);
    return fiber_interrupt(tid);
}

int fiber_stopped(fiber_t tid) {
    return (int) fiber::TaskGroup::is_stopped(tid);
}

fiber_t fiber_self(void) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    // note: return 0 for main tasks now, which include main thread and
    // all work threads. So that we can identify main tasks from logs
    // more easily. This is probably questionable in future.
    if (g != nullptr && !g->is_current_main_task()/*note*/) {
        return g->current_tid();
    }
    return INVALID_FIBER;
}

int fiber_equal(fiber_t t1, fiber_t t2) {
    return t1 == t2;
}

void fiber_exit(void *retval) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (g != nullptr && !g->is_current_main_task()) {
        throw fiber::ExitException(retval);
    } else {
        pthread_exit(retval);
    }
}

int fiber_join(fiber_t tid, void **thread_return) {
    return fiber::TaskGroup::join(tid, thread_return);
}

int fiber_attr_init(fiber_attr_t *a) {
    *a = FIBER_ATTR_NORMAL;
    return 0;
}

int fiber_attr_destroy(fiber_attr_t *) {
    return 0;
}

int fiber_getattr(fiber_t tid, fiber_attr_t *attr) {
    return fiber::TaskGroup::get_attr(tid, attr);
}

int fiber_getconcurrency(void) {
    return turbo::get_flag(FLAGS_fiber_concurrency);
}

int fiber_setconcurrency(int num) {
    if (num < FIBER_MIN_CONCURRENCY || num > FIBER_MAX_CONCURRENCY) {
        LOG(ERROR) << "Invalid concurrency=" << num;
        return EINVAL;
    }
    if (turbo::get_flag(FLAGS_fiber_min_concurrency) > 0) {
        if (num < turbo::get_flag(FLAGS_fiber_min_concurrency)) {
            return EINVAL;
        }
        if (fiber::never_set_fiber_concurrency) {
            fiber::never_set_fiber_concurrency = false;
        }
        turbo::set_flag(&FLAGS_fiber_concurrency,  num);
        return 0;
    }
    fiber::TaskControl *c = fiber::get_task_control();
    if (c != nullptr) {
        if (num < c->concurrency()) {
            return EPERM;
        } else if (num == c->concurrency()) {
            return 0;
        }
    }
    MELON_SCOPED_LOCK(fiber::g_task_control_mutex);
    c = fiber::get_task_control();
    if (c == nullptr) {
        if (fiber::never_set_fiber_concurrency) {
            fiber::never_set_fiber_concurrency = false;
            turbo::set_flag(&FLAGS_fiber_concurrency, num);
        } else if (num > turbo::get_flag(FLAGS_fiber_concurrency)) {
            turbo::set_flag(&FLAGS_fiber_concurrency , num);
        }
        return 0;
    }
    if (turbo::get_flag(FLAGS_fiber_concurrency) != c->concurrency()) {
        LOG(ERROR) << "CHECK failed: fiber_concurrency="
                   << turbo::get_flag(FLAGS_fiber_concurrency)
                   << " != tc_concurrency=" << c->concurrency();
        turbo::set_flag(&FLAGS_fiber_concurrency, c->concurrency());
    }
    if (num > turbo::get_flag(FLAGS_fiber_concurrency)) {
        // Create more workers if needed.
        auto added = fiber::add_workers_for_each_tag(num - turbo::get_flag(FLAGS_fiber_concurrency));
        auto nvalue = turbo::get_flag(FLAGS_fiber_concurrency) + added;
        turbo::set_flag(&FLAGS_fiber_concurrency, nvalue);
    }
    return (num == turbo::get_flag(FLAGS_fiber_concurrency) ? 0 : EPERM);
}

int fiber_getconcurrency_by_tag(fiber_tag_t tag) {
    MELON_SCOPED_LOCK(fiber::g_task_control_mutex);
    auto c = fiber::get_task_control();
    if (c == nullptr) {
        return EPERM;
    }
    return c->concurrency(tag);
}

int fiber_setconcurrency_by_tag(int num, fiber_tag_t tag) {
    if (fiber::never_set_fiber_concurrency_by_tag) {
        fiber::never_set_fiber_concurrency_by_tag = false;
        return 0;
    }
    MELON_SCOPED_LOCK(fiber::g_task_control_mutex);
    auto c = fiber::get_task_control();
    if (c == nullptr) {
        return EPERM;
    }
    auto ngroup = c->concurrency();
    auto tag_ngroup = c->concurrency(tag);
    auto add = num - tag_ngroup;
    if (ngroup + add > turbo::get_flag(FLAGS_fiber_concurrency)) {
        LOG(ERROR) << "Fail to set concurrency by tag " << tag
                   << ", Whole concurrency larger than fiber_concurrency";
        return EPERM;
    }
    auto added = 0;
    if (add > 0) {
        added = c->add_workers(add, tag);
        return (add == added ? 0 : EPERM);
    }
    return (num == tag_ngroup ? 0 : EPERM);
}

int fiber_about_to_quit() {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (g != nullptr) {
        fiber::TaskMeta *current_task = g->current_task();
        if (!(current_task->attr.flags & FIBER_NEVER_QUIT)) {
            current_task->about_to_quit = true;
        }
        return 0;
    }
    return EPERM;
}

int fiber_timer_add(fiber_timer_t *id, timespec abstime,
                    void (*on_timer)(void *), void *arg) {
    fiber::TaskControl *c = fiber::get_or_new_task_control();
    if (c == nullptr) {
        return ENOMEM;
    }
    fiber::TimerThread *tt = fiber::get_or_create_global_timer_thread();
    if (tt == nullptr) {
        return ENOMEM;
    }
    fiber_timer_t tmp = tt->schedule(on_timer, arg, abstime);
    if (tmp != 0) {
        *id = tmp;
        return 0;
    }
    return ESTOP;
}

int fiber_timer_del(fiber_timer_t id) {
    fiber::TaskControl *c = fiber::get_task_control();
    if (c != nullptr) {
        fiber::TimerThread *tt = fiber::get_global_timer_thread();
        if (tt == nullptr) {
            return EINVAL;
        }
        const int state = tt->unschedule(id);
        if (state >= 0) {
            return state;
        }
    }
    return EINVAL;
}

int fiber_usleep(uint64_t microseconds) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (nullptr != g && !g->is_current_pthread_task()) {
        return fiber::TaskGroup::usleep(&g, microseconds);
    }
    return ::usleep(microseconds);
}

int fiber_yield(void) {
    fiber::TaskGroup *g = fiber::tls_task_group;
    if (nullptr != g && !g->is_current_pthread_task()) {
        fiber::TaskGroup::yield(&g);
        return 0;
    }
    // pthread_yield is not available on MAC
    return sched_yield();
}

int fiber_set_worker_startfn(void (*start_fn)()) {
    if (start_fn == nullptr) {
        return EINVAL;
    }
    fiber::g_worker_startfn = start_fn;
    return 0;
}

int fiber_set_tagged_worker_startfn(void (*start_fn)(fiber_tag_t)) {
    if (start_fn == nullptr) {
        return EINVAL;
    }
    fiber::g_tagged_worker_startfn = start_fn;
    return 0;
}

void fiber_stop_world() {
    fiber::TaskControl *c = fiber::get_task_control();
    if (c != nullptr) {
        c->stop_and_join();
    }
}

int fiber_list_init(fiber_list_t *list,
                    unsigned /*size*/,
                    unsigned /*conflict_size*/) {
    list->impl = new(std::nothrow) fiber::TidList;
    if (nullptr == list->impl) {
        return ENOMEM;
    }
    // Set unused fields to zero as well.
    list->head = 0;
    list->size = 0;
    list->conflict_head = 0;
    list->conflict_size = 0;
    return 0;
}

void fiber_list_destroy(fiber_list_t *list) {
    delete static_cast<fiber::TidList *>(list->impl);
    list->impl = nullptr;
}

int fiber_list_add(fiber_list_t *list, fiber_t id) {
    if (list->impl == nullptr) {
        return EINVAL;
    }
    return static_cast<fiber::TidList *>(list->impl)->add(id);
}

int fiber_list_stop(fiber_list_t *list) {
    if (list->impl == nullptr) {
        return EINVAL;
    }
    static_cast<fiber::TidList *>(list->impl)->apply(fiber::TidStopper());
    return 0;
}

int fiber_list_join(fiber_list_t *list) {
    if (list->impl == nullptr) {
        return EINVAL;
    }
    static_cast<fiber::TidList *>(list->impl)->apply(fiber::TidJoiner());
    return 0;
}

fiber_tag_t fiber_self_tag(void) {
    return fiber::tls_task_group != nullptr ? fiber::tls_task_group->tag()
                                            : FIBER_TAG_DEFAULT;
}

}  // extern "C"
