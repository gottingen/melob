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


#include <unistd.h>                               // getpagesize
#include <sys/mman.h>                             // mmap, munmap, mprotect
#include <algorithm>                              // std::max
#include <cstdlib>                               // posix_memalign
#include <melon/utility/macros.h>                          // MELON_CASSERT
#include <melon/utility/memory/singleton_on_pthread_once.h>
#include <melon/utility/third_party/dynamic_annotations/dynamic_annotations.h> // RunningOnValgrind
#include <melon/utility/third_party/valgrind/valgrind.h>   // VALGRIND_STACK_REGISTER
#include <melon/var/passive_status.h>
#include <melon/fiber/types.h>                        // FIBER_STACKTYPE_*
#include <melon/fiber/stack.h>
#include <turbo/flags/flag.h>                        // FLAGS_*
#include <atomic>                               // std::atomic

TURBO_FLAG(int32_t, stack_size_small, 32768, "size of small stacks");
TURBO_FLAG(int32_t, stack_size_normal, 1048576, "size of normal stacks");
TURBO_FLAG(int32_t, stack_size_large, 8388608, "size of large stacks");
TURBO_FLAG(int32_t, guard_page_size, 4096, "size of guard page, allocate stacks by malloc if it's 0(not recommended)");
TURBO_FLAG(int32_t, tc_stack_small, 32, "maximum small stacks cached by each thread");
TURBO_FLAG(int32_t, tc_stack_normal, 8, "maximum normal stacks cached by each thread");


namespace fiber {


    static_assert(FIBER_STACKTYPE_PTHREAD == STACK_TYPE_PTHREAD, "must match");
    static_assert(FIBER_STACKTYPE_SMALL == STACK_TYPE_SMALL, "must match");
    static_assert(FIBER_STACKTYPE_NORMAL == STACK_TYPE_NORMAL, "must match");
    static_assert(FIBER_STACKTYPE_LARGE == STACK_TYPE_LARGE, "must match");
    static_assert(STACK_TYPE_MAIN == 0, "must ne 0");

    static std::atomic<int64_t> s_stack_count = MUTIL_STATIC_ATOMIC_INIT(0);

    static int64_t get_stack_count(void *) {
        return s_stack_count.load(std::memory_order_relaxed);
    }

    static melon::var::PassiveStatus<int64_t> var_stack_count(
            "fiber_stack_count", get_stack_count, nullptr);

    int allocate_stack_storage(StackStorage *s, int stacksize_in, int guardsize_in) {
        const static int PAGESIZE = getpagesize();
        const int PAGESIZE_M1 = PAGESIZE - 1;
        const int MIN_STACKSIZE = PAGESIZE * 2;
        const int MIN_GUARDSIZE = PAGESIZE;

        // Align stacksize
        const int stacksize =
                (std::max(stacksize_in, MIN_STACKSIZE) + PAGESIZE_M1) &
                ~PAGESIZE_M1;

        if (guardsize_in <= 0) {
            void *mem = malloc(stacksize);
            if (nullptr == mem) {
                PLOG_EVERY_N_SEC(ERROR, 1) << "Fail to malloc (size="
                                           << stacksize << ")";
                return -1;
            }
            s_stack_count.fetch_add(1, std::memory_order_relaxed);
            s->bottom = (char *) mem + stacksize;
            s->stacksize = stacksize;
            s->guardsize = 0;
            if (RunningOnValgrind()) {
                s->valgrind_stack_id = VALGRIND_STACK_REGISTER(
                        s->bottom, (char *) s->bottom - stacksize);
            } else {
                s->valgrind_stack_id = 0;
            }
            return 0;
        } else {
            // Align guardsize
            const int guardsize =
                    (std::max(guardsize_in, MIN_GUARDSIZE) + PAGESIZE_M1) &
                    ~PAGESIZE_M1;

            const int memsize = stacksize + guardsize;
            void *const mem = mmap(nullptr, memsize, (PROT_READ | PROT_WRITE),
                                   (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);

            if (MAP_FAILED == mem) {
                PLOG_EVERY_N_SEC(ERROR, 1)
                            << "Fail to mmap size=" << memsize << " stack_count="
                            << s_stack_count.load(std::memory_order_relaxed)
                            << ", possibly limited by /proc/sys/vm/max_map_count";
                // may fail due to limit of max_map_count (65536 in default)
                return -1;
            }

            void *aligned_mem = (void *) (((intptr_t) mem + PAGESIZE_M1) & ~PAGESIZE_M1);
            if (aligned_mem != mem) {
                LOG_FIRST_N(ERROR, 1) << "addr=" << mem << " returned by mmap is not "
                                                           "aligned by pagesize=" << PAGESIZE;
            }
            const int offset = (char *) aligned_mem - (char *) mem;
            if (guardsize <= offset ||
                mprotect(aligned_mem, guardsize - offset, PROT_NONE) != 0) {
                munmap(mem, memsize);
                PLOG_EVERY_N_SEC(ERROR, 1)
                            << "Fail to mprotect " << (void *) aligned_mem << " length="
                            << guardsize - offset;
                return -1;
            }

            s_stack_count.fetch_add(1, std::memory_order_relaxed);
            s->bottom = (char *) mem + memsize;
            s->stacksize = stacksize;
            s->guardsize = guardsize;
            if (RunningOnValgrind()) {
                s->valgrind_stack_id = VALGRIND_STACK_REGISTER(
                        s->bottom, (char *) s->bottom - stacksize);
            } else {
                s->valgrind_stack_id = 0;
            }
            return 0;
        }
    }

    void deallocate_stack_storage(StackStorage *s) {
        if (RunningOnValgrind()) {
            VALGRIND_STACK_DEREGISTER(s->valgrind_stack_id);
        }
        const int memsize = s->stacksize + s->guardsize;
        if ((uintptr_t) s->bottom <= (uintptr_t) memsize) {
            return;
        }
        s_stack_count.fetch_sub(1, std::memory_order_relaxed);
        if (s->guardsize <= 0) {
            free((char *) s->bottom - memsize);
        } else {
            munmap((char *) s->bottom - memsize, memsize);
        }
    }

    int SmallStackClass::stack_size_flag() {
        return turbo::get_flag(FLAGS_stack_size_small);
    }

    int NormalStackClass::stack_size_flag() {
        return turbo::get_flag(FLAGS_stack_size_normal);
    };

    int LargeStackClass::stack_size_flag() {
        return turbo::get_flag(FLAGS_stack_size_large);
    }

}  // namespace fiber
