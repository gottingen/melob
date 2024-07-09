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


#pragma once

#include <string>                       // std::string
#include <atomic>
#include <melon/utility/type_traits.h>
#include <melon/utility/synchronization/lock.h>
#include <melon/var/detail/is_atomical.h>
#include <melon/var/variable.h>
#include <melon/var/reducer.h>
#include <turbo/strings/str_format.h>

namespace melon::var {

    // Display a rarely or periodically updated value.
    // Usage:
    //   melon::var::Status<int> foo_count1(17);
    //   foo_count1.expose("my_value");
    //
    //   melon::var::Status<int> foo_count2;
    //   foo_count2.set_value(17);
    //
    //   melon::var::Status<int> foo_count3("my_value", 17);
    template<typename T, typename Enabler = void>
    class Status : public Variable {
    public:
        Status() {}

        Status(const T &value) : _value(value) {}

        Status(const std::string_view &name, const T &value) : _value(value) {
            this->expose(name);
        }

        Status(const std::string_view &prefix,
               const std::string_view &name, const T &value) : _value(value) {
            this->expose_as(prefix, name);
        }

        // Calling hide() manually is a MUST required by Variable.
        ~Status() { hide(); }

        void describe(std::ostream &os, bool /*quote_string*/) const override {
            os << get_value();
        }

        T get_value() const {
            mutil::AutoLock guard(_lock);
            const T res = _value;
            return res;
        }

        void set_value(const T &value) {
            mutil::AutoLock guard(_lock);
            _value = value;
        }

    private:
        T _value;
        // We use lock rather than std::atomic for generic values because
        // std::atomic requires the type to be memcpy-able (POD basically)
        mutable mutil::Lock _lock;
    };

    template<typename T>
    class Status<T, typename mutil::enable_if<detail::is_atomical<T>::value>::type>
            : public Variable {
    public:
        struct PlaceHolderOp {
            void operator()(T &, const T &) const {}
        };

        class SeriesSampler : public detail::Sampler {
        public:
            typedef typename mutil::conditional<
                    true, detail::AddTo<T>, PlaceHolderOp>::type Op;

            explicit SeriesSampler(Status *owner)
                    : _owner(owner), _series(Op()) {}

            void take_sample() { _series.append(_owner->get_value()); }

            void describe(std::ostream &os) { _series.describe(os, nullptr); }

        private:
            Status *_owner;
            detail::Series<T, Op> _series;
        };

    public:
        Status() : _series_sampler(nullptr) {}

        Status(const T &value) : _value(value), _series_sampler(nullptr) {}

        Status(const std::string_view &name, const T &value)
                : _value(value), _series_sampler(nullptr) {
            this->expose(name);
        }

        Status(const std::string_view &prefix,
               const std::string_view &name, const T &value)
                : _value(value), _series_sampler(nullptr) {
            this->expose_as(prefix, name);
        }

        ~Status() {
            hide();
            if (_series_sampler) {
                _series_sampler->destroy();
                _series_sampler = nullptr;
            }
        }

        void describe(std::ostream &os, bool /*quote_string*/) const override {
            os << get_value();
        }


        T get_value() const {
            return _value.load(std::memory_order_relaxed);
        }

        void set_value(const T &value) {
            _value.store(value, std::memory_order_relaxed);
        }

        int describe_series(std::ostream &os, const SeriesOptions &options) const override {
            if (_series_sampler == nullptr) {
                return 1;
            }
            if (!options.test_only) {
                _series_sampler->describe(os);
            }
            return 0;
        }

    protected:
        int expose_impl(const std::string_view &prefix,
                        const std::string_view &name,
                        DisplayFilter display_filter) override {
            const int rc = Variable::expose_impl(prefix, name, display_filter);
            if (rc == 0 &&
                _series_sampler == nullptr &&
                turbo::get_flag(FLAGS_save_series)) {
                _series_sampler = new SeriesSampler(this);
                _series_sampler->schedule();
            }
            return rc;
        }

    private:
        std::atomic<T> _value;
        SeriesSampler *_series_sampler;
    };

    // Specialize for std::string, adding a printf-style set_value().
    template<>
    class Status<std::string, void> : public Variable {
    public:
        Status() {}

        template<typename... Args>
        Status(const std::string_view &name, const turbo::FormatSpec<Args...> &fmt, const Args &... args) {
            _value = turbo::str_format(fmt, &_value, args...);
            expose(name);
        }

        template<typename... Args>
        Status(const std::string_view &prefix,
               const std::string_view &name, const turbo::FormatSpec<Args...> &fmt, const Args &... args) {
            _value = turbo::str_format(fmt, &_value, args...);
            expose_as(prefix, name);
        }

        ~Status() { hide(); }

        void describe(std::ostream &os, bool quote_string) const override {
            if (quote_string) {
                os << '"' << get_value() << '"';
            } else {
                os << get_value();
            }
        }

        std::string get_value() const {
            mutil::AutoLock guard(_lock);
            return _value;
        }

        template<typename... Args>
        void set_value(const turbo::FormatSpec<Args...> &fmt, const Args &... args) {

            mutil::AutoLock guard(_lock);
            _value = turbo::str_format(fmt, &_value, args...);

        }

        void set_value(const std::string &s) {
            mutil::AutoLock guard(_lock);
            _value = s;
        }

    private:
        std::string _value;
        mutable mutil::Lock _lock;
    };

}  // namespace melon::var
