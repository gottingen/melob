/*

    libcontext - a slightly more portable version of boost::context

    Copyright Martin Husemann 2013.
    Copyright Oliver Kowalke 2009.
    Copyright Sergue E. Leontiev 2013.
    Copyright Thomas Sailer 2013.
    Minor modifications by Tomasz Wlostowski 2016.

 Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)

*/

#ifndef MELON_FIBER_INTERNAL_CONTEXT_H_
#define MELON_FIBER_INTERNAL_CONTEXT_H_

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__APPLE__)

#define MELON_FIBER_CONTEXT_COMPILER_gcc

#if defined(__linux__)
#ifdef __x86_64__
#define MELON_FIBER_CONTEXT_PLATFORM_linux_x86_64
#define MELON_FIBER_CONTEXT_CALL_CONVENTION

#elif __i386__
#define MELON_FIBER_CONTEXT_PLATFORM_linux_i386
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#elif __arm__
#define MELON_FIBER_CONTEXT_PLATFORM_linux_arm32
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#elif __aarch64__
#define MELON_FIBER_CONTEXT_PLATFORM_linux_arm64
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#endif

#elif defined(__MINGW32__) || defined (__MINGW64__)
#if defined(__x86_64__)
#define MELON_FIBER_CONTEXT_COMPILER_gcc
#define MELON_FIBER_CONTEXT_PLATFORM_windows_x86_64
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#endif

#if defined(__i386__)
#define MELON_FIBER_CONTEXT_COMPILER_gcc
#define MELON_FIBER_CONTEXT_PLATFORM_windows_i386
#define MELON_FIBER_CONTEXT_CALL_CONVENTION __cdecl
#endif
#elif defined(__APPLE__) && defined(__MACH__)
#if defined (__i386__)
#define MELON_FIBER_CONTEXT_PLATFORM_apple_i386
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#elif defined (__x86_64__)
#define MELON_FIBER_CONTEXT_PLATFORM_apple_x86_64
#define MELON_FIBER_CONTEXT_CALL_CONVENTION
#endif
#endif
#endif

#if defined(_WIN32_WCE)
typedef int intptr_t;
#endif

typedef void *fiber_context_type;

#ifdef __cplusplus
extern "C" {
#endif

intptr_t MELON_FIBER_CONTEXT_CALL_CONVENTION
melon_fiber_jump_context(fiber_context_type *ofc, fiber_context_type nfc,
                         intptr_t vp, bool preserve_fpu = false);
fiber_context_type MELON_FIBER_CONTEXT_CALL_CONVENTION
melon_fiber_make_context(void *sp, size_t size, void (*fn)(intptr_t));

#ifdef __cplusplus
};
#endif

#endif  // MELON_FIBER_INTERNAL_CONTEXT_H_
