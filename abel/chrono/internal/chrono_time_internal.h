// Copyright (c) 2021, gottingen group.
// All rights reserved.
// Created by liyinbin lijippy@163.com

#ifndef ABEL_CHRONO_INTERNAL_CHRONO_TIME_INTERNAL_H_
#define ABEL_CHRONO_INTERNAL_CHRONO_TIME_INTERNAL_H_

#include "abel/base/profile.h"
#include "abel/chrono/internal/chrono_time_detail.h"

namespace abel {
namespace chrono_internal {


using civil_year = detail::civil_year;
using civil_month = detail::civil_month;
using civil_day = detail::civil_day;
using civil_hour = detail::civil_hour;
using civil_minute = detail::civil_minute;
using civil_second = detail::civil_second;

// An enum class with members monday, tuesday, wednesday, thursday, friday,
// saturday, and sunday. These enum values may be sent to an output stream
// using operator<<(). The result is the full weekday name in English with a
// leading capital letter.
//
//   weekday wd = weekday::thursday;
//   std::cout << wd << "\n";  // Outputs: Thursday
//
using detail::weekday;

// Returns the weekday for the given civil-time value.
//
//   civil_day a(2015, 8, 13);
//   weekday wd = get_weekday(a);  // wd == weekday::thursday
//
using detail::get_weekday;

// Returns the civil_day that strictly follows or precedes the given
// civil_day, and that falls on the given weekday.
//
// For example, given:
//
//     August 2015
// Su Mo Tu We Th Fr Sa
//                    1
//  2  3  4  5  6  7  8
//  9 10 11 12 13 14 15
// 16 17 18 19 20 21 22
// 23 24 25 26 27 28 29
// 30 31
//
//   civil_day a(2015, 8, 13);  // get_weekday(a) == weekday::thursday
//   civil_day b = next_weekday(a, weekday::thursday);  // b = 2015-08-20
//   civil_day c = prev_weekday(a, weekday::thursday);  // c = 2015-08-06
//
//   civil_day d = ...
//   // Gets the following Thursday if d is not already Thursday
//   civil_day thurs1 = next_weekday(d - 1, weekday::thursday);
//   // Gets the previous Thursday if d is not already Thursday
//   civil_day thurs2 = prev_weekday(d + 1, weekday::thursday);
//
using detail::next_weekday;
using detail::prev_weekday;

// Returns the day-of-year for the given civil-time value.
//
//   civil_day a(2015, 1, 1);
//   int yd_jan_1 = get_yearday(a);   // yd_jan_1 = 1
//   civil_day b(2015, 12, 31);
//   int yd_dec_31 = get_yearday(b);  // yd_dec_31 = 365
//
using detail::get_yearday;

}  // chrono_internal
}  // abel

#endif  // ABEL_CHRONO_INTERNAL_CHRONO_TIME_INTERNAL_H_
