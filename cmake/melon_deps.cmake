#
# Copyright 2023 The Carbin Authors.
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
############################################################
# system pthread and rt, dl
############################################################
#[[
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DYNAMIC_LIB ${DYNAMIC_LIB} rt)
    set(MELON_PRIVATE_LIBS "${MELON_PRIVATE_LIBS} -lrt")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(DYNAMIC_LIB ${DYNAMIC_LIB}
            pthread
            "-framework CoreFoundation"
            "-framework CoreGraphics"
            "-framework CoreData"
            "-framework CoreText"
            "-framework Security"
            "-framework Foundation"
            "-Wl,-U,_MallocExtension_ReleaseFreeMemory"
            "-Wl,-U,_ProfilerStart"
            "-Wl,-U,_ProfilerStop"
            "-Wl,-U,__Z13GetStackTracePPvii")
endif ()
]]
find_package(Threads REQUIRED)
set(CARBIN_SYSTEM_DYLINK)
if (APPLE)
    find_library(CoreFoundation CoreFoundation)
    list(APPEND CARBIN_SYSTEM_DYLINK ${CoreFoundation} pthread)
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    list(APPEND CARBIN_SYSTEM_DYLINK rt dl pthread)
endif ()

if (CARBIN_BUILD_TEST)
    enable_testing()
    #include(require_gtest)
    #include(require_gmock)
    #include(require_doctest)
endif (CARBIN_BUILD_TEST)

if (CARBIN_BUILD_BENCHMARK)
    #include(require_benchmark)
endif ()

set(CARBIN_DEPS_INCLUDE "")
############################################################
# leveldb
############################################################
carbin_find_static_leveldb()
if (NOT LEVELDB_STATIC_FOUND)
    message(FATAL_ERROR "Fail to find leveldb")
endif ()
list(APPEND CARBIN_DEPS_INCLUDE ${LEVELDB_INCLUDE_DIR})
############################################################
# protobuf
############################################################
find_package(Protobuf REQUIRED)
carbin_print("Protobuf include dir: ${PROTOBUF_INCLUDE_DIRS}")
carbin_print("Protobuf libraries: ${PROTOBUF_LIBRARY}")
carbin_print("Protobuf protoc: ${PROTOBUF_PROTOC_EXECUTABLE}")
carbin_print("Protobuf protoc: ${PROTOBUF_PROTOC_LIBRARY}")

list(APPEND CARBIN_DEPS_INCLUDE ${PROTOBUF_INCLUDE_DIRS})

############################################################
# openssl using static openssl
############################################################
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL include dir: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL ssl library: ${OPENSSL_SSL_LIBRARY}")
message(STATUS "OpenSSL crypto library: ${OPENSSL_CRYPTO_LIBRARY}")

list(APPEND CARBIN_DEPS_INCLUDE ${OPENSSL_INCLUDE_DIR})

############################################################
# zlib
############################################################
macro(melon_find_zlib)
    set(ZLIB_FOUND FALSE)
    find_path(ZLIB_INCLUDE_DIR NAMES zlib.h)
    find_library(ZLIB_LIB NAMES z libz.a)
    if (ZLIB_LIB AND ZLIB_INCLUDE_DIR)
        set(ZLIB_FOUND TRUE)
    endif ()
    carbin_print("ZLIB_FOUND: ${ZLIB_FOUND}")
    if (ZLIB_FOUND)
        carbin_print("ZLIB_INCLUDE_DIR: ${ZLIB_INCLUDE_DIR}")
        carbin_print("ZLIB_LIB: ${ZLIB_LIB}")
    endif ()
endmacro()
macro(melon_find_snappy)
    set(SNAPPY_FOUND FALSE)
    find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h)
    find_library(SNAPPY_LIB NAMES snappy libsnappy.a)
    if (SNAPPY_LIB AND SNAPPY_INCLUDE_DIR)
        set(SNAPPY_FOUND TRUE)
    endif ()
    carbin_print("SNAPPY_FOUND: ${SNAPPY_FOUND}")
    if (SNAPPY_FOUND)
        carbin_print("SNAPPY_INCLUDE_DIR: ${SNAPPY_INCLUDE_DIR}")
        carbin_print("SNAPPY_LIB: ${SNAPPY_LIB}")
    endif ()
endmacro()

melon_find_zlib()
if (NOT ZLIB_FOUND)
    message(FATAL_ERROR "Fail to find zlib")
endif ()
list(APPEND CARBIN_DEPS_INCLUDE ${ZLIB_INCLUDE_DIR})

melon_find_snappy()
if (NOT SNAPPY_FOUND)
    message(FATAL_ERROR "Fail to find snappy")
endif ()
############################################################
# turbo
############################################################
find_package(turbo 0.5.6 REQUIRED)
get_target_property(TURBO_STATIC_LIB turbo::turbo_static LOCATION)
carbin_print("Turbo static lib: ${TURBO_STATIC_LIB}")
############################################################
#
# add you libs to the CARBIN_DEPS_LINK variable eg as turbo
# so you can and system pthread and rt, dl already add to
# CARBIN_SYSTEM_DYLINK, using it for fun.
##########################################################
set(MELON_DEPS_LINK
        ${PROTOBUF_LIBRARY}
        ${PROTOBUF_PROTOC_LIBRARY}
        ${LEVELDB_STATIC_LIB}
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${ZLIB_LIB}
        ${SNAPPY_LIB}
        ${TURBO_STATIC_LIB}
        ${CARBIN_SYSTEM_DYLINK}
        )
list(REMOVE_DUPLICATES MELON_DEPS_LINK)
list(REMOVE_DUPLICATES CARBIN_DEPS_INCLUDE)
include_directories(${CARBIN_DEPS_INCLUDE})
carbin_print_list_label("Denpendcies:" MELON_DEPS_LINK)
carbin_print_list_label("Denpendcies:" CARBIN_DEPS_INCLUDE)





