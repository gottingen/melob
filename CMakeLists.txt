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
####################################################################################
# generate by carbin package manager tools for titan-search
# https://github.com/gottingen/carbin
####################################################################################
cmake_minimum_required(VERSION 3.24)

project(melon CXX C ASM)

option(${PROJECT_NAME}_IS_TOP_PROJECT "melon project is the top level project or not" ON)

get_property(not_top DIRECTORY PROPERTY PARENT_DIRECTORY)
if (not_top)
    set(${PROJECT_NAME}_IS_TOP_PROJECT OFF)
endif ()
message(STATUS ${PROJECT_NAME}_IS_TOP_PROJECT)

set(PROJECT_DESCRIPTION "melon is a c++ rpc framework, it's a high performance, easy to use, and powerful rpc framework")
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 6)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

set(${PROJECT_NAME}_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(${PROJECT_NAME}_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(${PROJECT_NAME}_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(${PROJECT_NAME}_VERSION ${PROJECT_VERSION})

string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UP)

set(CARBIN_VERSION 0.6.0)
set(CARBIN_CMAKE_VERSION 0.6.0)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/carbin_cmake)
##################################################################
# add this for ${PROJECT_NAME} 's cmake include
##################################################################
list(APPEND CMAKE_MODULE_PATH ${${PROJECT_NAME}_SOURCE_DIR}/cmake)
#################################################################
#
# modify the CARBIN_DEPS_PREFIX to you prefix if needed.
# for example when you install you deps to "/opt/handsome/guy
# modify as set(CARBIN_DEPS_PREFIX /opt/handsome/guy)
# this should be PROJECT_SOURCE_DIR, only install libraries
# deps to top level project
#################################################################
set(CARBIN_DEPS_PREFIX ${PROJECT_SOURCE_DIR}/carbin)
#######################################################################
# warning
# in carbin_cmake directory, caution to modify files in that dir
######################################################################
include(carbin_module)

##################################################################
#
# your cmake directory ${PROJECT_SOURCE_DIR}/cmake, do things here
#################################################################
include(melon_deps)
include(melon_cxx_config)
include(melon_test)
configure_file(${PROJECT_SOURCE_DIR}/conda/meta.yaml.in ${PROJECT_SOURCE_DIR}/conda/meta.yaml @ONLY)
string(TOUPPER ${CMAKE_BUILD_TYPE} UPPERCASE_BUILD_TYPE)
configure_file(${PROJECT_SOURCE_DIR}/melon/version.h.in ${PROJECT_SOURCE_DIR}/melon/version.h @ONLY)
#################################################################
# proto library
#################################################################

include(CompileProto)
set(PROTO_FILES
        melon/proto/idl_options.proto
        melon/proto/rpc/rtmp.proto
        melon/proto/rpc/rpc_dump.proto
        melon/proto/rpc/sns.proto
        melon/proto/rpc/get_favicon.proto
        melon/proto/rpc/span.proto
        melon/proto/rpc/builtin.proto
        melon/proto/rpc/builtin_service.proto
        melon/proto/rpc/grpc_health_check.proto
        melon/proto/rpc/get_js.proto
        melon/proto/rpc/errno.proto
        melon/proto/rpc/options.proto
        melon/proto/rpc/melon_rpc_meta.proto
        melon/proto/rpc/hulu_pbrpc_meta.proto
        melon/proto/rpc/public_pbrpc_meta.proto
        melon/proto/rpc/mongo.proto
        melon/proto/rpc/restful.proto
        melon/proto/rpc/trackme.proto
        melon/proto/rpc/streaming_rpc_meta.proto
        melon/proto/rpc/proto_base.proto
        melon/proto/rpc/webui.proto
        melon/proto/raft/builtin_service.proto
        melon/proto/raft/cli.proto
        melon/proto/raft/enum.proto
        melon/proto/raft/errno.proto
        melon/proto/raft/file_service.proto
        melon/proto/raft/local_file_meta.proto
        melon/proto/raft/local_storage.proto
        melon/proto/raft/raft.proto
)
file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/output/include/melon/rpc)
set(PROTOC_FLAGS ${PROTOC_FLAGS} -I${PROTOBUF_INCLUDE_DIR})
compile_proto(PROTO_HDRS PROTO_SRCS ${PROJECT_BINARY_DIR}
        ${PROJECT_BINARY_DIR}/output/include
        ${PROJECT_SOURCE_DIR}
        "${PROTO_FILES}")
carbin_cc_object(
        NAMESPACE melon
        NAME proto
        SOURCES
        ${PROTO_SRCS}
        CXXOPTS
        ${MELON_CXX_OPTIONS}
)
#################################################################
# melon library
#################################################################
add_subdirectory(melon)
add_subdirectory(tools)
####################################################################
# belows are auto, edit it be cation
####################################################################
if (CARBIN_BUILD_TEST)
    set(TEST_PROTO_FILES
            test/addressbook1.proto
            test/addressbook_encode_decode.proto
            test/addressbook_map.proto
            test/addressbook.proto
            test/echo.proto
            test/iobuf.proto
            test/message.proto
            test/repeated.proto
            test/snappy_message.proto
            test/v1.proto
            test/v2.proto
            test/grpc.proto
            test/health_check.proto)
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/test/hdrs)
    set(PROTOC_FLAGS ${PROTOC_FLAGS} -I${PROJECT_SOURCE_DIR})
    compile_proto(TEST_PROTO_HDRS TEST_PROTO_SRCS ${PROJECT_BINARY_DIR}
            ${PROJECT_BINARY_DIR}/test/hdrs
            ${PROJECT_SOURCE_DIR}
            "${TEST_PROTO_FILES}")
    carbin_cc_test_library(
            NAMESPACE melon
            NAME test_proto
            SOURCES
            ${TEST_PROTO_SRCS}
            CXXOPTS
            ${MELON_CXX_OPTIONS}
    )
    enable_testing()
    add_subdirectory(test)
endif ()

if (CARBIN_BUILD_BENCHMARK)
    #add_subdirectory(benchmark)
endif ()

if (CARBIN_BUILD_EXAMPLES)
    #add_subdirectory(examples)
endif ()

##############################################
# header installing
#############################################
install(DIRECTORY ${PROJECT_NAME}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING
        PATTERN "*.inc"
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.proto"
        )
#install(DIRECTORY ${PROJECT_SOURCE_DIR}/example DESTINATION share/melon)

# Layout. This works for all platforms:
#   * <prefix>/lib*/cmake/<PROJECT-NAME>
#   * <prefix>/lib*/
#   * <prefix>/include/
set(config_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")

set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Configuration
set(version_config "${generated_dir}/${PROJECT_NAME}ConfigVersion.cmake")
set(project_config "${generated_dir}/${PROJECT_NAME}Config.cmake")
set(TARGETS_EXPORT_NAME "${PROJECT_NAME}Targets")
set(namespace "${PROJECT_NAME}::")

# Include module with fuction 'write_basic_package_version_file'
include(CMakePackageConfigHelpers)

# Configure '<PROJECT-NAME>ConfigVersion.cmake'
# Use:
#   * PROJECT_VERSION
write_basic_package_version_file(
        "${version_config}" COMPATIBILITY SameMajorVersion
)

# Configure '<PROJECT-NAME>Config.cmake'
# Use variables:
#   * TARGETS_EXPORT_NAME
#   * PROJECT_NAME
configure_package_config_file(
        "cmake/melon_config.cmake.in"
        "${project_config}"
        INSTALL_DESTINATION "${config_install_dir}"
)

#Targets:
#  * <prefix>/lib/libbar.a
#  * <prefix>/lib/libbaz.a
#  * header location after install: <prefix>/include/foo/Bar.hpp
#  * headers can be included by C++ code `#include <foo/Bar.hpp>`


# Config
#   * <prefix>/lib/cmake/Foo/FooConfig.cmake
#   * <prefix>/lib/cmake/Foo/FooConfigVersion.cmake
install(
        FILES
        "${project_config}" "${version_config}"
        DESTINATION
        "${config_install_dir}"
)

# Config
#   * <prefix>/lib/cmake/Foo/FooTargets.cmake

install(
        EXPORT
        "${TARGETS_EXPORT_NAME}"
        NAMESPACE
        "${namespace}"
        DESTINATION
        "${config_install_dir}"
)
configure_file(${PROJECT_SOURCE_DIR}/carbin_cmake/package/pkg_dump_template.pc.in
        ${PROJECT_BINARY_DIR}/package/${PROJECT_NAME}.pc)

install(FILES ${PROJECT_BINARY_DIR}/package/${PROJECT_NAME}.pc DESTINATION lib/pkgconfig)
include(melon_cpack_config)
carbin_print("melon cmake build system is created by carbin package manager tools")
carbin_print("carbin: https://github.com/gottingen/carbin")
carbin_print("template: https://github.com/gottingen/carbin-template@master")