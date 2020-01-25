#ifndef ABEL_FORMAT_INTERNAL_BIND_H_
#define ABEL_FORMAT_INTERNAL_BIND_H_

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <abel/base/profile.h>
#include <abel/format/internal/arg.h>
#include <abel/format/internal/checker.h>
#include <abel/format/internal/parser.h>
#include <abel/types/span.h>

namespace abel {

class untyped_format_spec;

namespace format_internal {

class BoundConversion : public conversion_spec {
public:
    const FormatArgImpl *arg () const { return arg_; }
    void set_arg (const FormatArgImpl *a) { arg_ = a; }

private:
    const FormatArgImpl *arg_;
};

// This is the type-erased class that the implementation uses.
class UntypedFormatSpecImpl {
public:
    UntypedFormatSpecImpl () = delete;

    explicit UntypedFormatSpecImpl (string_view s)
        : data_(s.data()), size_(s.size()) { }
    explicit UntypedFormatSpecImpl (
        const format_internal::parsed_format_base *pc)
        : data_(pc), size_(~size_t {}) { }

    bool has_parsed_conversion () const { return size_ == ~size_t {}; }

    string_view str () const {
        assert(!has_parsed_conversion());
        return string_view(static_cast<const char *>(data_), size_);
    }
    const format_internal::parsed_format_base *parsed_conversion () const {
        assert(has_parsed_conversion());
        return static_cast<const format_internal::parsed_format_base *>(data_);
    }

    template<typename T>
    static const UntypedFormatSpecImpl &Extract (const T &s) {
        return s.spec_;
    }

private:
    const void *data_;
    size_t size_;
};

template<typename T, typename...>
struct MakeDependent {
    using type = T;
};

// Implicitly convertible from `const char*`, `string_view`, and the
// `extended_parsed_format` type. This abstraction allows all format functions to
// operate on any without providing too many overloads.
template<typename... Args>
class FormatSpecTemplate
    : public MakeDependent<untyped_format_spec, Args...>::type {
    using Base = typename MakeDependent<untyped_format_spec, Args...>::type;

public:
#ifdef ABEL_INTERNAL_ENABLE_FORMAT_CHECKER

    // Honeypot overload for when the std::string is not constexpr.
    // We use the 'unavailable' attribute to give a better compiler error than
    // just 'method is deleted'.
    FormatSpecTemplate (...)  // NOLINT
    __attribute__((unavailable("Format std::string is not constexpr.")));

    // Honeypot overload for when the format is constexpr and invalid.
    // We use the 'unavailable' attribute to give a better compiler error than
    // just 'method is deleted'.
    // To avoid checking the format twice, we just check that the format is
    // constexpr. If is it valid, then the overload below will kick in.
    // We add the template here to make this overload have lower priority.
    template<typename = void>
    FormatSpecTemplate (const char *s)  // NOLINT
    __attribute__((
    enable_if(format_internal::ensure_constexpr(s), "constexpr trap"),
    unavailable(
    "Format specified does not match the arguments passed.")));

    template<typename T = void>
    FormatSpecTemplate (string_view s)  // NOLINT
    __attribute__((enable_if(format_internal::ensure_constexpr(s),
    "constexpr trap"))) {
        static_assert(sizeof(T *) == 0,
                      "Format specified does not match the arguments passed.");
    }

    // Good format overload.
    FormatSpecTemplate (const char *s)  // NOLINT
    __attribute__((enable_if(valid_format_impl<argument_to_conv<Args>()...>(s),
    "bad format trap")))
        : Base(s) { }

    FormatSpecTemplate (string_view s)  // NOLINT
    __attribute__((enable_if(valid_format_impl<argument_to_conv<Args>()...>(s),
    "bad format trap")))
        : Base(s) { }

#else  // ABEL_INTERNAL_ENABLE_FORMAT_CHECKER

    FormatSpecTemplate(const char* s) : Base(s) {}  // NOLINT
    FormatSpecTemplate(string_view s) : Base(s) {}  // NOLINT

#endif  // ABEL_INTERNAL_ENABLE_FORMAT_CHECKER

    template<format_conv... C, typename = typename std::enable_if<
        sizeof...(C) == sizeof...(Args) &&
            all_of(conv_contains(argument_to_conv<Args>(),
                           C)...)>::type>
    FormatSpecTemplate (const extended_parsed_format<C...> &pc)  // NOLINT
        : Base(&pc) { }
};

template<typename... Args>
struct FormatSpecDeductionBarrier {
    using type = FormatSpecTemplate<Args...>;
};

class Streamable {
public:
    Streamable (const UntypedFormatSpecImpl &format,
                abel::Span<const FormatArgImpl> args)
        : format_(format) {
        if (args.size() <= ABEL_ARRAYSIZE(few_args_)) {
            for (size_t i = 0; i < args.size(); ++i) {
                few_args_[i] = args[i];
            }
            args_ = abel::MakeSpan(few_args_, args.size());
        } else {
            many_args_.assign(args.begin(), args.end());
            args_ = many_args_;
        }
    }

    std::ostream &Print (std::ostream &os) const;

    friend std::ostream &operator << (std::ostream &os, const Streamable &l) {
        return l.Print(os);
    }

private:
    const UntypedFormatSpecImpl &format_;
    abel::Span<const FormatArgImpl> args_;
    // if args_.size() is 4 or less:
    FormatArgImpl few_args_[4] = {FormatArgImpl(0), FormatArgImpl(0),
                                  FormatArgImpl(0), FormatArgImpl(0)};
    // if args_.size() is more than 4:
    std::vector<FormatArgImpl> many_args_;
};

// for testing
std::string Summarize (UntypedFormatSpecImpl format,
                       abel::Span<const FormatArgImpl> args);
bool BindWithPack (const unbound_conversion *props,
                   abel::Span<const FormatArgImpl> pack, BoundConversion *bound);

bool FormatUntyped (format_raw_sink_impl raw_sink,
                    UntypedFormatSpecImpl format,
                    abel::Span<const FormatArgImpl> args);

std::string &AppendPack (std::string *out, UntypedFormatSpecImpl format,
                         abel::Span<const FormatArgImpl> args);

std::string FormatPack (const UntypedFormatSpecImpl format,
                        abel::Span<const FormatArgImpl> args);

int FprintF (std::FILE *output, UntypedFormatSpecImpl format,
             abel::Span<const FormatArgImpl> args);
int SnprintF (char *output, size_t size, UntypedFormatSpecImpl format,
              abel::Span<const FormatArgImpl> args);

// Returned by Streamed(v). Converts via '%s' to the std::string created
// by std::ostream << v.
template<typename T>
class StreamedWrapper {
public:
    explicit StreamedWrapper (const T &v) : v_(v) { }

private:
    template<typename S>
    friend convert_result<format_conv::s> FormatConvertImpl (const StreamedWrapper<S> &v,
                                                     conversion_spec conv,
                                                     format_sink_impl *out);
    const T &v_;
};

}  // namespace format_internal

}  // namespace abel

#endif  // ABEL_FORMAT_INTERNAL_BIND_H_
