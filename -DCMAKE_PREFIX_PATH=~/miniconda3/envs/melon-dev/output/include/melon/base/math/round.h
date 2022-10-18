
/****************************************************************
 * Copyright (c) 2022, liyinbin
 * All rights reserved.
 * Author by liyinbin (jeff.li) lijippy@163.com
 *****************************************************************/

#ifndef MELON_BASE_MATH_ROUND_H_
#define MELON_BASE_MATH_ROUND_H_

#include <cmath>
#include "melon/base/profile.h"

namespace melon::base {

    /// Can't use std::round() because it is only available in C++11.
    /// Note that we ignore the possibility of floating-point over/underflow.
    template<typename Double>
    MELON_FORCE_INLINE double round(Double d) {
        return d < 0 ? std::ceil(d - 0.5) : std::floor(d + 0.5);
    }
}  // namespace melon::base

#endif  // MELON_BASE_MATH_ROUND_H_
