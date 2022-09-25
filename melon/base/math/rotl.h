
/****************************************************************
 * Copyright (c) 2022, liyinbin
 * All rights reserved.
 * Author by liyinbin (jeff.li) lijippy@163.com
 *****************************************************************/

#ifndef MELON_BASE_MATH_ROTL_H_
#define MELON_BASE_MATH_ROTL_H_

#include <cstdint>
#include <cstdlib>
#include "melon/base/profile.h"

namespace melon::base {


    /// rotl() - rotate bits left in 32-bit integers
    /// rotl - generic implementation
    static MELON_FORCE_INLINE uint32_t rotl_generic(const uint32_t &x, int i) {
        return (x << static_cast<uint32_t>(i & 31)) |
               (x >> static_cast<uint32_t>((32 - (i & 31)) & 31));
    }

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))

    /// rotl - gcc/clang assembler
    static MELON_FORCE_INLINE uint32_t rotl(const uint32_t &x, int i) {
        uint32_t x1 = x;
        asm ("roll %%cl,%0" : "=r" (x1) : "0" (x1), "c" (i));
        return x1;
    }

#elif defined(_MSC_VER)

    /// rotl - MSVC intrinsic
    static MELON_FORCE_INLINE uint32_t rotl(const uint32_t& x, int i) {
        return _rotl(x, i);
    }

#else

    /// rotl - generic
    MELON_FORCE_INLINE uint32_t rotl(const uint32_t& x, int i) {
        return rotl_generic(x, i);
    }

#endif


    /// rotl() - rotate bits left in 64-bit integers
    /// rotl - generic implementation
    static MELON_FORCE_INLINE uint64_t rotl_generic(const uint64_t &x, int i) {
        return (x << static_cast<uint64_t>(i & 63)) |
               (x >> static_cast<uint64_t>((64 - (i & 63)) & 63));
    }

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)

    /// rotl - gcc/clang assembler
    static MELON_FORCE_INLINE uint64_t rotl(const uint64_t &x, int i) {
        uint64_t x1 = x;
        asm ("rolq %%cl,%0" : "=r" (x1) : "0" (x1), "c" (i));
        return x1;
    }

#else

    /// rotl - generic
    static MELON_FORCE_INLINE uint64_t rotl(const uint64_t& x, int i) {
        return rotl_generic(x, i);
    }

#endif
}  // namespace melon::base

#endif  // MELON_BASE_MATH_ROTL_H_
