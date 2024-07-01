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

#include <melon/raft/util.h>
#include <turbo/flags/flag.h>
#include <stdlib.h>
#include <melon/utility/macros.h>
#include <melon/utility/raw_pack.h>                     // mutil::RawPacker
#include <melon/utility/file_util.h>
#include <melon/raft/raft.h>
#include <turbo/strings/match.h>

// Reloading following does not change names of the corresponding vars.
// Avoid reloading in practice.
TURBO_DECLARE_FLAG(int32_t,var_counter_p3);
TURBO_DECLARE_FLAG(int32_t,var_counter_p2);
TURBO_FLAG(int32_t, var_counter_p1, 80, "First counter percentile").on_validate(
        [](std::string_view value, std::string *err) noexcept -> bool {
            int32_t val;
            if (!turbo::parse_flag(value, &val, err)) {
                return false;
            }
            if (val <= 0 || val >= 100) {
                if (err)
                    *err = "Percentile must be in (0, 100)";
                return false;
            }
            if (val >= turbo::get_flag(FLAGS_var_counter_p2)) {
                if (err)
                    *err = "Percentile must be smaller than FLAGS_var_counter_p2";
                return false;
            }
            return true;
        });
TURBO_FLAG(int32_t,var_counter_p2, 90, "Second counter percentile").on_validate(
        [](std::string_view value, std::string *err) noexcept -> bool {
            int32_t val;
            if (!turbo::parse_flag(value, &val, err)) {
                return false;
            }
            if (val <= 0 || val >= 100) {
                if (err)
                    *err = "Percentile must be in (0, 100)";
                return false;
            }
            if (val <= turbo::get_flag(FLAGS_var_counter_p1)) {
                if (err)
                    *err = "Percentile must be larger than FLAGS_var_counter_p1";
                return false;
            }
            if (val >= turbo::get_flag(FLAGS_var_counter_p3)) {
                if (err)
                    *err = "Percentile must be larger than FLAGS_var_counter_p1";
                return false;
            }
            return true;
        });
TURBO_FLAG(int32_t,var_counter_p3, 99, "Third counter percentile").on_validate(
        [](std::string_view value, std::string *err) noexcept -> bool {
            int32_t val;
            if (!turbo::parse_flag(value, &val, err)) {
                return false;
            }
            if (val <= 0 || val >= 100) {
                if (err)
                    *err = "Percentile must be in (0, 100)";
                return false;
            }
            if (val <= turbo::get_flag(FLAGS_var_counter_p2)) {
                if (err)
                    *err = "Percentile must be larger than FLAGS_var_counter_p2";
                return false;
            }
            return true;
        });

namespace melon::var {

    namespace detail {

        typedef PercentileSamples<1022> CombinedPercentileSamples;

        static int64_t get_window_recorder_qps(void *arg) {
            detail::Sample<Stat> s;
            static_cast<RecorderWindow *>(arg)->get_span(1, &s);
            // Use floating point to avoid overflow.
            if (s.time_us <= 0) {
                return 0;
            }
            return static_cast<int64_t>(round(s.data.num * 1000000.0 / s.time_us));
        }

        static int64_t get_recorder_count(void *arg) {
            return static_cast<IntRecorder *>(arg)->get_value().num;
        }

// Caller is responsible for deleting the return value.
        static CombinedPercentileSamples *combine(PercentileWindow *w) {
            CombinedPercentileSamples *cb = new CombinedPercentileSamples;
            std::vector<GlobalPercentileSamples> buckets;
            w->get_samples(&buckets);
            cb->combine_of(buckets.begin(), buckets.end());
            return cb;
        }

        template<int64_t numerator, int64_t denominator>
        static int64_t get_counter_percetile(void *arg) {
            return ((CounterRecorder *) arg)->counter_percentile(
                    (double) numerator / double(denominator));
        }

        static int64_t get_p1_counter(void *arg) {
            CounterRecorder *cr = static_cast<CounterRecorder *>(arg);
            return cr->counter_percentile(turbo::get_flag(FLAGS_var_counter_p1) / 100.0);
        }

        static int64_t get_p2_counter(void *arg) {
            CounterRecorder *cr = static_cast<CounterRecorder *>(arg);
            return cr->counter_percentile(turbo::get_flag(FLAGS_var_counter_p2) / 100.0);
        }

        static int64_t get_p3_counter(void *arg) {
            CounterRecorder *cr = static_cast<CounterRecorder *>(arg);
            return cr->counter_percentile(turbo::get_flag(FLAGS_var_counter_p3) / 100.0);
        }

        static Vector<int64_t, 4> get_counters(void *arg) {
            std::unique_ptr<CombinedPercentileSamples> cb(
                    combine((PercentileWindow *) arg));
            // NOTE: We don't show 99.99% since it's often significantly larger than
            // other values and make other curves on the plotted graph small and
            // hard to read.ggggnnn
            Vector<int64_t, 4> result;
            result[0] = cb->get_number(turbo::get_flag(FLAGS_var_counter_p1) / 100.0);
            result[1] = cb->get_number(turbo::get_flag(FLAGS_var_counter_p2) / 100.0);
            result[2] = cb->get_number(turbo::get_flag(FLAGS_var_counter_p3) / 100.0);
            result[3] = cb->get_number(0.999);
            return result;
        }

        CounterRecorderBase::CounterRecorderBase(time_t window_size)
                : _max_counter(), _avg_counter_window(&_avg_counter, window_size),
                  _max_counter_window(&_max_counter, window_size),
                  _counter_percentile_window(&_counter_percentile, window_size),
                  _total_times(get_recorder_count, &_avg_counter), _qps(get_window_recorder_qps, &_avg_counter_window),
                  _counter_p1(get_p1_counter, this), _counter_p2(get_p2_counter, this),
                  _counter_p3(get_p3_counter, this), _counter_999(get_counter_percetile<999, 1000>, this),
                  _counter_9999(get_counter_percetile<9999, 10000>, this), _counter_cdf(&_counter_percentile_window),
                  _counter_percentiles(get_counters, &_counter_percentile_window) {}

    }  // namespace detail

// CounterRecorder
    Vector<int64_t, 4> CounterRecorder::counter_percentiles() const {
        // const_cast here is just to adapt parameter type and safe.
        return detail::get_counters(
                const_cast<detail::PercentileWindow *>(&_counter_percentile_window));
    }

    int64_t CounterRecorder::qps(time_t window_size) const {
        detail::Sample<Stat> s;
        _avg_counter_window.get_span(window_size, &s);
        // Use floating point to avoid overflow.
        if (s.time_us <= 0) {
            return 0;
        }
        return static_cast<int64_t>(round(s.data.num * 1000000.0 / s.time_us));
    }

    int CounterRecorder::expose(const std::string_view &prefix1,
                                const std::string_view &prefix2) {
        if (prefix2.empty()) {
            LOG(ERROR) << "Parameter[prefix2] is empty";
            return -1;
        }
        std::string_view prefix = prefix2;
        // User may add "_counter" as the suffix, remove it.
        if (turbo::starts_with(prefix, "counter") || turbo::starts_with(prefix, "Counter")) {
            prefix.remove_suffix(7);
            if (prefix.empty()) {
                LOG(ERROR) << "Invalid prefix2=" << prefix2;
                return -1;
            }
        }
        std::string tmp;
        if (!prefix1.empty()) {
            tmp.reserve(prefix1.size() + prefix.size() + 1);
            tmp.append(prefix1.data(), prefix1.size());
            tmp.push_back('_'); // prefix1 ending with _ is good.
            tmp.append(prefix.data(), prefix.size());
            prefix = tmp;
        }

        // set debug names for printing helpful error log.
        _avg_counter.set_debug_name(prefix);
        _counter_percentile.set_debug_name(prefix);

        if (_avg_counter_window.expose_as(prefix, "avg_counter") != 0) {
            return -1;
        }
        if (_max_counter_window.expose_as(prefix, "max_counter") != 0) {
            return -1;
        }
        if (_total_times.expose_as(prefix, "total_times") != 0) {
            return -1;
        }
        if (_qps.expose_as(prefix, "qps") != 0) {
            return -1;
        }
        char namebuf[32];
        snprintf(namebuf, sizeof(namebuf), "counter_%d", (int) turbo::get_flag(FLAGS_var_counter_p1));
        if (_counter_p1.expose_as(prefix, namebuf, DISPLAY_ON_PLAIN_TEXT) != 0) {
            return -1;
        }
        snprintf(namebuf, sizeof(namebuf), "counter_%d", (int) turbo::get_flag(FLAGS_var_counter_p2));
        if (_counter_p2.expose_as(prefix, namebuf, DISPLAY_ON_PLAIN_TEXT) != 0) {
            return -1;
        }
        snprintf(namebuf, sizeof(namebuf), "counter_%u", (int) turbo::get_flag(FLAGS_var_counter_p3));
        if (_counter_p3.expose_as(prefix, namebuf, DISPLAY_ON_PLAIN_TEXT) != 0) {
            return -1;
        }
        if (_counter_999.expose_as(prefix, "counter_999", DISPLAY_ON_PLAIN_TEXT) != 0) {
            return -1;
        }
        if (_counter_9999.expose_as(prefix, "counter_9999") != 0) {
            return -1;
        }
        if (_counter_cdf.expose_as(prefix, "counter_cdf", DISPLAY_ON_HTML) != 0) {
            return -1;
        }
        if (_counter_percentiles.expose_as(prefix, "counter_percentiles", DISPLAY_ON_HTML) != 0) {
            return -1;
        }
        snprintf(namebuf, sizeof(namebuf), "%d%%,%d%%,%d%%,99.9%%",
                 (int) turbo::get_flag(FLAGS_var_counter_p1), (int) turbo::get_flag(FLAGS_var_counter_p2),
                 (int) turbo::get_flag(FLAGS_var_counter_p3));
        CHECK_EQ(0, _counter_percentiles.set_vector_names(namebuf));
        return 0;
    }

    int64_t CounterRecorder::counter_percentile(double ratio) const {
        std::unique_ptr<detail::CombinedPercentileSamples> cb(
        combine((detail::PercentileWindow *) &_counter_percentile_window));
        return cb->get_number(ratio);
    }

    void CounterRecorder::hide() {
        _avg_counter_window.hide();
        _max_counter_window.hide();
        _total_times.hide();
        _qps.hide();
        _counter_p1.hide();
        _counter_p2.hide();
        _counter_p3.hide();
        _counter_999.hide();
        _counter_9999.hide();
        _counter_cdf.hide();
        _counter_percentiles.hide();
    }

    CounterRecorder &CounterRecorder::operator<<(int64_t count_num) {
        _avg_counter << count_num;
        _max_counter << count_num;
        _counter_percentile << count_num;
        return *this;
    }

    std::ostream &operator<<(std::ostream &os, const CounterRecorder &rec) {
        return os << "{avg=" << rec.avg_counter()
                  << " max" << rec.window_size() << '=' << rec.max_counter()
                  << " qps=" << rec.qps()
                  << " count=" << rec.total_times() << '}';
    }

}  // namespace melon::var


namespace melon::raft {

    static void *run_closure(void *arg) {
        ::google::protobuf::Closure *c = (google::protobuf::Closure *) arg;
        if (c) {
            c->Run();
        }
        return NULL;
    }

    void run_closure_in_fiber(google::protobuf::Closure *closure,
                                bool in_pthread) {
        DCHECK(closure);
        fiber_t tid;
        fiber_attr_t attr = (in_pthread)
                              ? FIBER_ATTR_PTHREAD : FIBER_ATTR_NORMAL;
        int ret = fiber_start_background(&tid, &attr, run_closure, closure);
        if (0 != ret) {
            PLOG(ERROR) << "Fail to start fiber";
            return closure->Run();
        }
    }

    void run_closure_in_fiber_nosig(google::protobuf::Closure *closure,
                                      bool in_pthread) {
        DCHECK(closure);
        fiber_t tid;
        fiber_attr_t attr = (in_pthread)
                              ? FIBER_ATTR_PTHREAD : FIBER_ATTR_NORMAL;
        attr = attr | FIBER_NOSIGNAL;
        int ret = fiber_start_background(&tid, &attr, run_closure, closure);
        if (0 != ret) {
            PLOG(ERROR) << "Fail to start fiber";
            return closure->Run();
        }
    }

    ssize_t file_pread(mutil::IOPortal *portal, int fd, off_t offset, size_t size) {
        off_t orig_offset = offset;
        ssize_t left = size;
        while (left > 0) {
            ssize_t read_len = portal->pappend_from_file_descriptor(
                    fd, offset, static_cast<size_t>(left));
            if (read_len > 0) {
                left -= read_len;
                offset += read_len;
            } else if (read_len == 0) {
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG(WARNING) << "read failed, err: " << berror()
                             << " fd: " << fd << " offset: " << orig_offset << " size: " << size;
                return -1;
            }
        }

        return size - left;
    }

    ssize_t file_pwrite(const mutil::IOBuf &data, int fd, off_t offset) {
        size_t size = data.size();
        mutil::IOBuf piece_data(data);
        off_t orig_offset = offset;
        ssize_t left = size;
        while (left > 0) {
            ssize_t written = piece_data.pcut_into_file_descriptor(fd, offset, left);
            if (written >= 0) {
                offset += written;
                left -= written;
            } else if (errno == EINTR) {
                continue;
            } else {
                LOG(WARNING) << "write falied, err: " << berror()
                             << " fd: " << fd << " offset: " << orig_offset << " size: " << size;
                return -1;
            }
        }

        return size - left;
    }

    void FileSegData::append(const mutil::IOBuf &data, uint64_t offset) {
        uint32_t len = data.size();
        if (0 != _seg_offset && offset == (_seg_offset + _seg_len)) {
            // append to last segment
            _seg_len += len;
            _data.append(data);
        } else {
            // close last segment
            char seg_header[sizeof(uint64_t) + sizeof(uint32_t)] = {0};
            if (_seg_len > 0) {
                ::mutil::RawPacker(seg_header).pack64(_seg_offset).pack32(_seg_len);
                CHECK_EQ(0, _data.unsafe_assign(_seg_header, seg_header));
            }

            // start new segment
            _seg_offset = offset;
            _seg_len = len;
            _seg_header = _data.reserve(sizeof(seg_header));
            CHECK(_seg_header != mutil::IOBuf::INVALID_AREA);
            _data.append(data);
        }
    }

    void FileSegData::append(void *data, uint64_t offset, uint32_t len) {
        if (0 != _seg_offset && offset == (_seg_offset + _seg_len)) {
            // append to last segment
            _seg_len += len;
            _data.append(data, len);
        } else {
            // close last segment
            char seg_header[sizeof(uint64_t) + sizeof(uint32_t)] = {0};
            if (_seg_len > 0) {
                ::mutil::RawPacker(seg_header).pack64(_seg_offset).pack32(_seg_len);
                CHECK_EQ(0, _data.unsafe_assign(_seg_header, seg_header));
            }

            // start new segment
            _seg_offset = offset;
            _seg_len = len;
            _seg_header = _data.reserve(sizeof(seg_header));
            CHECK(_seg_header != mutil::IOBuf::INVALID_AREA);
            _data.append(data, len);
        }
    }

    void FileSegData::close() {
        char seg_header[sizeof(uint64_t) + sizeof(uint32_t)] = {0};
        if (_seg_len > 0) {
            ::mutil::RawPacker(seg_header).pack64(_seg_offset).pack32(_seg_len);
            CHECK_EQ(0, _data.unsafe_assign(_seg_header, seg_header));
        }

        _seg_offset = 0;
        _seg_len = 0;
    }

    size_t FileSegData::next(uint64_t *offset, mutil::IOBuf *data) {
        data->clear();
        if (_data.length() == 0) {
            return 0;
        }

        char header_buf[sizeof(uint64_t) + sizeof(uint32_t)] = {0};
        size_t header_len = _data.cutn(header_buf, sizeof(header_buf));
        CHECK_EQ(header_len, sizeof(header_buf)) << "header_len: " << header_len;

        uint64_t seg_offset = 0;
        uint32_t seg_len = 0;
        ::mutil::RawUnpacker(header_buf).unpack64(seg_offset).unpack32(seg_len);

        *offset = seg_offset;
        size_t body_len = _data.cutn(data, seg_len);
        CHECK_EQ(body_len, seg_len) << "seg_len: " << seg_len << " body_len: " << body_len;
        return seg_len;
    }

}  //  namespace melon::raft
