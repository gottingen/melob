
/****************************************************************
 * Copyright (c) 2022, liyinbin
 * All rights reserved.
 * Author by liyinbin (jeff.li) lijippy@163.com
 *****************************************************************/
#include <string.h>
#include <cctype>
#include <cstdint>

#include "melon/times/internal/time_zone.h"
#include "melon/times/time.h"

namespace melon {

    extern const char RFC3339_full[] = "%Y-%m-%dT%H:%M:%E*S%Ez";
    extern const char RFC3339_sec[] = "%Y-%m-%dT%H:%M:%S%Ez";

    extern const char RFC1123_full[] = "%a, %d %b %E4Y %H:%M:%S %z";
    extern const char RFC1123_no_wday[] = "%d %b %E4Y %H:%M:%S %z";

    namespace {

        const char kInfiniteFutureStr[] = "infinite-future";
        const char kInfinitePastStr[] = "infinite-past";

        struct cctz_parts {
            melon::times_internal::time_point<melon::times_internal::seconds> sec;
            melon::times_internal::times_detail::femtoseconds fem;
        };

        MELON_FORCE_INLINE melon::times_internal::time_point<melon::times_internal::seconds>
        unix_epoch() {
            return std::chrono::time_point_cast<melon::times_internal::seconds>(
                    std::chrono::system_clock::from_time_t(0));
        }

        // Splits a time_point into seconds and femtoseconds, which can be used with CCTZ.
        // Requires that 't' is finite. See duration.cc for details about rep_hi and
        // rep_lo.
        cctz_parts Split(melon::time_point t) {
            const auto d = time_point::to_unix_duration(t);
            const int64_t rep_hi = duration::get_rep_hi(d);
            const int64_t rep_lo = duration::get_rep_lo(d);
            const auto sec = unix_epoch() + melon::times_internal::seconds(rep_hi);
            const auto fem = melon::times_internal::times_detail::femtoseconds(rep_lo * (1000 * 1000 / 4));
            return {sec, fem};
        }

        // Joins the given seconds and femtoseconds into a time_point. See duration.cc for
        // details about rep_hi and rep_lo.
        melon::time_point Join(const cctz_parts &parts) {
            const int64_t rep_hi = (parts.sec - unix_epoch()).count();
            const uint32_t rep_lo = parts.fem.count() / (1000 * 1000 / 4);
            const auto d = duration::make_duration(rep_hi, rep_lo);
            return time_point::from_unix_duration(d);
        }

    }  // namespace

    std::string format_time(const std::string &format, melon::time_point t,
                            melon::time_zone tz) {
        if (t == melon::time_point::infinite_future())
            return kInfiniteFutureStr;
        if (t == melon::time_point::infinite_past())
            return kInfinitePastStr;
        const auto parts = Split(t);
        return melon::times_internal::times_detail::format(format, parts.sec, parts.fem,
                                                     melon::times_internal::time_zone(tz));
    }

    std::string format_time(melon::time_point t, melon::time_zone tz) {
        return format_time(RFC3339_full, t, tz);
    }

    std::string format_time(melon::time_point t) {
        return melon::format_time(RFC3339_full, t, melon::local_time_zone());
    }

    bool parse_time(const std::string &format, const std::string &input,
                    melon::time_point *time, std::string *err) {
        return melon::parse_time(format, input, melon::utc_time_zone(), time, err);
    }

    // If the input string does not contain an explicit UTC offset, interpret
    // the fields with respect to the given time_zone.
    bool parse_time(const std::string &format, const std::string &input,
                    melon::time_zone tz, melon::time_point *time, std::string *err) {
        const char *data = input.c_str();
        while (std::isspace(*data))
            ++data;

        size_t inf_size = strlen(kInfiniteFutureStr);
        if (strncmp(data, kInfiniteFutureStr, inf_size) == 0) {
            const char *new_data = data + inf_size;
            while (std::isspace(*new_data))
                ++new_data;
            if (*new_data == '\0') {
                *time = time_point::infinite_future();
                return true;
            }
        }

        inf_size = strlen(kInfinitePastStr);
        if (strncmp(data, kInfinitePastStr, inf_size) == 0) {
            const char *new_data = data + inf_size;
            while (std::isspace(*new_data))
                ++new_data;
            if (*new_data == '\0') {
                *time = time_point::infinite_past();
                return true;
            }
        }

        std::string error;
        cctz_parts parts;
        const bool b = melon::times_internal::times_detail::parse(format, input, melon::times_internal::time_zone(tz),
                                                            &parts.sec, &parts.fem, &error);
        if (b) {
            *time = Join(parts);
        } else if (err != nullptr) {
            *err = error;
        }
        return b;
    }


}  // namespace melon
