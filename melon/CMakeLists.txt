#
# Copyright 2023 The titan-search Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set(UTILITY_SOURCES
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/dmg_fp/g_fmt.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/dmg_fp/dtoa_wrapper.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/dynamic_annotations/dynamic_annotations.c
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/icu/icu_utf.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/superfasthash/superfasthash.c
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/modp_b64/modp_b64.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/symbolize/demangle.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/symbolize/symbolize.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/third_party/murmurhash3/murmurhash3.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/arena.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/at_exit.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/atomicops_internals_x86_gcc.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/cpu.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/alias.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/asan_invalid_access.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/crash_logging.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/debugger.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/debugger_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/dump_without_crashing.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/proc_maps_linux.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/stack_trace.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/debug/stack_trace_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/environment.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_enumerator.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_enumerator_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_path.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_path_constants.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/memory_mapped_file.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/memory_mapped_file_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/scoped_file.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/files/scoped_temp_dir.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/file_util.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/file_util_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/hash.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/lazy_instance.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/memory/aligned_memory.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/memory/ref_counted.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/memory/ref_counted_memory.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/memory/singleton.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/memory/weak_ptr.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/posix/file_descriptor_shuffle.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/posix/global_descriptors.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/process_util.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/rand_util.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/rand_util_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/fast_rand.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/safe_strerror_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/latin1_string_conversions.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/nullable_string16.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/safe_sprintf.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string16.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string_number_conversions.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string_split.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string_piece.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string_util.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/string_util_constants.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/stringprintf.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/utf_offset_string_conversions.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/utf_string_conversion_utils.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/strings/utf_string_conversions.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/synchronization/cancellation_flag.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/synchronization/condition_variable_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/synchronization/waitable_event_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/non_thread_safe_impl.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/platform_thread_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/simple_thread.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_checker_impl.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_collision_warner.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_id_name_manager.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_local_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_local_storage.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_local_storage_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/thread_restrictions.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/threading/watchdog.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/clock.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/default_clock.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/default_tick_clock.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/tick_clock.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/time.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/time/time_posix.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/class_name.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/errno.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/find_cstr.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/status.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/string_printf.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/thread_local.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/thread_key.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/unix_socket.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/endpoint.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/fd_utility.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/files/temp_file.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/files/file_watcher.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/time.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/zero_copy_stream_as_streambuf.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/crc32c.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/containers/case_ignored_flat_map.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/iobuf.cc
        ${PROJECT_SOURCE_DIR}/melon/utility/binary_printer.cpp
        ${PROJECT_SOURCE_DIR}/melon/utility/popen.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(UTILITY_SOURCES ${UTILITY_SOURCES}
            ${PROJECT_SOURCE_DIR}/melon/utility/file_util_linux.cc
            ${PROJECT_SOURCE_DIR}/melon/utility/threading/platform_thread_linux.cc
            ${PROJECT_SOURCE_DIR}/melon/utility/strings/sys_string_conversions_posix.cc)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(UTILITY_SOURCES ${UTILITY_SOURCES}
            ${PROJECT_SOURCE_DIR}/melon/utility/mac/bundle_locations.mm
            ${PROJECT_SOURCE_DIR}/melon/utility/mac/foundation_util.mm
            ${PROJECT_SOURCE_DIR}/melon/utility/file_util_mac.mm
            ${PROJECT_SOURCE_DIR}/melon/utility/threading/platform_thread_mac.mm
            ${PROJECT_SOURCE_DIR}/melon/utility/strings/sys_string_conversions_mac.mm
            ${PROJECT_SOURCE_DIR}/melon/utility/time/time_mac.cc
            ${PROJECT_SOURCE_DIR}/melon/utility/mac/scoped_mach_port.cc)
endif ()

file(GLOB_RECURSE VAR_SOURCES "${PROJECT_SOURCE_DIR}/melon/var/*.cc")
file(GLOB_RECURSE FIBER_SOURCES "${PROJECT_SOURCE_DIR}/melon/fiber/*.cc")
file(GLOB_RECURSE JSON2PB_SOURCES "${PROJECT_SOURCE_DIR}/melon/json2pb/*.cc")
file(GLOB_RECURSE CORE_SOURCES "${PROJECT_SOURCE_DIR}/melon/core/*.cc")
file(GLOB_RECURSE RPC_SOURCES "${PROJECT_SOURCE_DIR}/melon/rpc/*.cc")
file(GLOB_RECURSE NAMING_SOURCES "${PROJECT_SOURCE_DIR}/melon/naming/*.cc")
file(GLOB_RECURSE LB_SOURCES "${PROJECT_SOURCE_DIR}/melon/lb/*.cc")
file(GLOB_RECURSE BUILTIN_SOURCES "${PROJECT_SOURCE_DIR}/melon/builtin/*.cc")
file(GLOB_RECURSE BR_SOURCES "${PROJECT_SOURCE_DIR}/melon/br/*.cc")
file(GLOB_RECURSE COMPRESS_SOURCES "${PROJECT_SOURCE_DIR}/melon/compress/*.cc")
file(GLOB_RECURSE RAFT_SOURCES "${PROJECT_SOURCE_DIR}/melon/raft/*.cc")
file(GLOB_RECURSE EXCLUDE_SOURCES "${PROJECT_SOURCE_DIR}/melon/rpc/event_dispatcher_*.cc")


foreach (v ${EXCLUDE_SOURCES})
    list(REMOVE_ITEM RPC_SOURCES ${v})
endforeach ()

set(SOURCES
        ${VAR_SOURCES}
        ${FIBER_SOURCES}
        ${JSON2PB_SOURCES}
        ${CORE_SOURCES}
        ${RPC_SOURCES}
        ${NAMING_SOURCES}
        ${LB_SOURCES}
        ${COMPRESS_SOURCES}
        ${BUILTIN_SOURCES}
        ${BR_SOURCES}
        ${RAFT_SOURCES}
)

carbin_cc_object(
        NAMESPACE melon
        NAME utility
        SOURCES
        ${UTILITY_SOURCES}
        DEFINES ${MELON_CXX_DEFINES}
        CXXOPTS
        ${MELON_CXX_OPTIONS}
        COPTS
        ${MELON_C_OPTIONS}
)

carbin_cc_object(
        NAMESPACE melon
        NAME sources
        SOURCES
        ${SOURCES}
        DEPS
        proto
        DEFINES ${MELON_CXX_DEFINES}
        CXXOPTS
        ${MELON_CXX_OPTIONS}
        COPTS
        ${MELON_C_OPTIONS}
)

carbin_cc_library(
        NAMESPACE melon
        NAME melon
        OBJECTS utility sources proto
        DEFINES ${MELON_CXX_DEFINES}
        CXXOPTS
        ${CARBIN_CXX_OPTIONS}
        DEPS
        utility proto sources
        PLINKS
        ${MELON_DEPS_LINK}
        PUBLIC
)
if (CARBIN_BUILD_TEST)
    carbin_cc_object(
            NAMESPACE melon
            NAME utility_test
            SOURCES
            ${UTILITY_SOURCES}
            DEFINES ${MELON_TEST_DEFINES}
            CXXOPTS
            ${MELON_CXX_TEST_OPTIONS}
            COPTS
            ${MELON_C_OPTIONS}
    )
    carbin_cc_library(
            NAMESPACE melon
            NAME var_test_lib
            SOURCES
            ${VAR_SOURCES}
            OBJECTS utility_test
            DEPS
            utility_test
            DEFINES ${MELON_TEST_DEFINES}
            CXXOPTS
            ${MELON_CXX_TEST_OPTIONS}
            COPTS
            ${MELON_C_OPTIONS}
    )

    carbin_cc_object(
            NAMESPACE melon
            NAME sources_test
            SOURCES
            ${SOURCES}
            DEFINES ${MELON_TEST_DEFINES}
            CXXOPTS
            ${MELON_CXX_TEST_OPTIONS}
            COPTS
            ${MELON_C_OPTIONS}
    )

    carbin_cc_library(
            NAMESPACE melon
            NAME melon_test
            OBJECTS utility_test sources_test proto
            CXXOPTS
            ${MELON_TEST_CXX_OPTIONS}
            DEFINES ${MELON_TEST_DEFINES}
            DEPS
            utility_test proto sources_test
            PLINKS
            ${MELON_DEPS_LINK}
    )

endif ()