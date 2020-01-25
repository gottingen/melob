#ifndef ABEL_STRINGS_STR_FORMAT_H_
#define ABEL_STRINGS_STR_FORMAT_H_

#include <cstdio>
#include <string>

#include <abel/format/internal/arg.h>  // IWYU pragma: export
#include <abel/format/internal//bind.h>  // IWYU pragma: export
#include <abel/format/internal/checker.h>  // IWYU pragma: export
#include <abel/format/internal/format_conv.h>  // IWYU pragma: export
#include <abel/format/internal/parser.h>  // IWYU pragma: export

namespace abel {

// untyped_format_spec
//
// A type-erased class that can be used directly within untyped API entry
// points. An `untyped_format_spec` is specifically used as an argument to
// `FormatUntyped()`.
//
// Example:
//
//   abel::untyped_format_spec format("%d");
//   std::string out;
//   CHECK(abel::FormatUntyped(&out, format, {abel::format_arg(1)}));
class untyped_format_spec {
public:
    untyped_format_spec () = delete;
    untyped_format_spec (const untyped_format_spec &) = delete;
    untyped_format_spec &operator = (const untyped_format_spec &) = delete;

    explicit untyped_format_spec (string_view s) : _spec(s) { }

    const format_internal::untyped_format_spec_impl& get_spec() const { return _spec; }
protected:
    explicit untyped_format_spec (const format_internal::parsed_format_base *pc)
        : _spec(pc) { }

private:
    format_internal::untyped_format_spec_impl _spec;
};

// FormatStreamed()
//
// Takes a streamable argument and returns an object that can print it
// with '%s'. Allows printing of types that have an `operator<<` but no
// intrinsic type support within `string_format()` itself.
//
// Example:
//
//   abel::string_format("%s", abel::FormatStreamed(obj));
template<typename T>
format_internal::streamed_wrapper<T> FormatStreamed (const T &v) {
    return format_internal::streamed_wrapper<T>(v);
}

// format_count_capture
//
// This class provides a way to safely wrap `string_format()` captures of `%n`
// conversions, which denote the number of characters written by a formatting
// operation to this point, into an integer value.
//
// This wrapper is designed to allow safe usage of `%n` within `string_format(); in
// the `string_printf()` family of functions, `%n` is not safe to use, as the `int *`
// buffer can be used to capture arbitrary data.
//
// Example:
//
//   int n = 0;
//   std::string s = abel::string_format("%s%d%n", "hello", 123,
//                       abel::format_count_capture(&n));
//   EXPECT_EQ(8, n);
class format_count_capture {
public:
    explicit format_count_capture (int *p) : p_(p) { }

private:
    // format_count_capture_helper is used to define format_convert_impl() for this
    // class.
    friend struct format_internal::format_count_capture_helper;
    // Unused() is here because of the false positive from -Wunused-private-field
    // p_ is used in the templated function of the friend format_count_capture_helper
    // class.
    int *Unused () { return p_; }
    int *p_;
};

// format_spec
//
// The `format_spec` type defines the makeup of a format string within the
// `str_format` library. It is a variadic class template that is evaluated at
// compile-time, according to the format string and arguments that are passed to
// it.
//
// You should not need to manipulate this type directly. You should only name it
// if you are writing wrapper functions which accept format arguments that will
// be provided unmodified to functions in this library. Such a wrapper function
// might be a class method that provides format arguments and/or internally uses
// the result of formatting.
//
// For a `format_spec` to be valid at compile-time, it must be provided as
// either:
//
// * A `constexpr` literal or `abel::string_view`, which is how it most often
//   used.
// * A `parsed_format` instantiation, which ensures the format string is
//   valid before use. (See below.)
//
// Example:
//
//   // Provided as a string literal.
//   abel::string_format("Welcome to %s, Number %d!", "The Village", 6);
//
//   // Provided as a constexpr abel::string_view.
//   constexpr abel::string_view formatString = "Welcome to %s, Number %d!";
//   abel::string_format(formatString, "The Village", 6);
//
//   // Provided as a pre-compiled parsed_format object.
//   // Note that this example is useful only for illustration purposes.
//   abel::parsed_format<'s', 'd'> formatString("Welcome to %s, Number %d!");
//   abel::string_format(formatString, "TheVillage", 6);
//
// A format string generally follows the POSIX syntax as used within the POSIX
// `string_printf` specification.
//
// (See http://pubs.opengroup.org/onlinepubs/9699919799/functions/fprintf.html.)
//
// In specific, the `format_spec` supports the following type specifiers:
//   * `c` for characters
//   * `s` for strings
//   * `d` or `i` for integers
//   * `o` for unsigned integer conversions into octal
//   * `x` or `X` for unsigned integer conversions into hex
//   * `u` for unsigned integers
//   * `f` or `F` for floating point values into decimal notation
//   * `e` or `E` for floating point values into exponential notation
//   * `a` or `A` for floating point values into hex exponential notation
//   * `g` or `G` for floating point values into decimal or exponential
//     notation based on their precision
//   * `p` for pointer address values
//   * `n` for the special case of writing out the number of characters
//     written to this point. The resulting value must be captured within an
//     `abel::format_count_capture` type.
//
// Implementation-defined behavior:
//   * A null pointer provided to "%s" or "%p" is output as "(nil)".
//   * A non-null pointer provided to "%p" is output in hex as if by %#x or
//     %#lx.
//
// NOTE: `o`, `x\X` and `u` will convert signed values to their unsigned
// counterpart before formatting.
//
// Examples:
//     "%c", 'a'                -> "a"
//     "%c", 32                 -> " "
//     "%s", "C"                -> "C"
//     "%s", std::string("C++") -> "C++"
//     "%d", -10                -> "-10"
//     "%o", 10                 -> "12"
//     "%x", 16                 -> "10"
//     "%f", 123456789          -> "123456789.000000"
//     "%e", .01                -> "1.00000e-2"
//     "%a", -3.0               -> "-0x1.8p+1"
//     "%g", .01                -> "1e-2"
//     "%p", (void*)&value      -> "0x7ffdeb6ad2a4"
//
//     int n = 0;
//     std::string s = abel::string_format(
//         "%s%d%n", "hello", 123, abel::format_count_capture(&n));
//     EXPECT_EQ(8, n);
//
// The `format_spec` intrinsically supports all of these fundamental C++ types:
//
// *   Characters: `char`, `signed char`, `unsigned char`
// *   Integers: `int`, `short`, `unsigned short`, `unsigned`, `long`,
//         `unsigned long`, `long long`, `unsigned long long`
// *   Floating-point: `float`, `double`, `long double`
//
// However, in the `str_format` library, a format conversion specifies a broader
// C++ conceptual category instead of an exact type. For example, `%s` binds to
// any string-like argument, so `std::string`, `abel::string_view`, and
// `const char*` are all accepted. Likewise, `%d` accepts any integer-like
// argument, etc.

template<typename... Args>
using format_spec =
typename format_internal::format_spec_deduction_barrier<Args...>::type;

// parsed_format
//
// A `parsed_format` is a class template representing a preparsed `format_spec`,
// with template arguments specifying the conversion characters used within the
// format string. Such characters must be valid format type specifiers, and
// these type specifiers are checked at compile-time.
//
// Instances of `parsed_format` can be created, copied, and reused to speed up
// formatting loops. A `parsed_format` may either be constructed statically, or
// dynamically through its `New()` factory function, which only constructs a
// runtime object if the format is valid at that time.
//
// Example:
//
//   // Verified at compile time.
//   abel::parsed_format<'s', 'd'> formatString("Welcome to %s, Number %d!");
//   abel::string_format(formatString, "TheVillage", 6);
//
//   // Verified at runtime.
//   auto format_runtime = abel::parsed_format<'d'>::New(format_string);
//   if (format_runtime) {
//     value = abel::string_format(*format_runtime, i);
//   } else {
//     ... error case ...
//   }
template<char... format_conv>
using parsed_format = format_internal::extended_parsed_format<
    format_internal::conversion_char_to_conv(format_conv)...>;

// string_format()
//
// Returns a `string` given a `string_printf()`-style format string and zero or more
// additional arguments. Use it as you would `sprintf()`. `string_format()` is the
// primary formatting function within the `str_format` library, and should be
// used in most cases where you need type-safe conversion of types into
// formatted strings.
//
// The format string generally consists of ordinary character data along with
// one or more format conversion specifiers (denoted by the `%` character).
// Ordinary character data is returned unchanged into the result string, while
// each conversion specification performs a type substitution from
// `string_format()`'s other arguments. See the comments for `format_spec` for full
// information on the makeup of this format string.
//
// Example:
//
//   std::string s = abel::string_format(
//       "Welcome to %s, Number %d!", "The Village", 6);
//   EXPECT_EQ("Welcome to The Village, Number 6!", s);
//
// Returns an empty string in case of error.
template<typename... Args>
ABEL_MUST_USE_RESULT std::string string_format (const format_spec<Args...> &format,
                                                const Args &... args) {
    return format_internal::format_pack(
        format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// string_append_format()
//
// Appends to a `dst` string given a format string, and zero or more additional
// arguments, returning `*dst` as a convenience for chaining purposes. Appends
// nothing in case of error (but possibly alters its capacity).
//
// Example:
//
//   std::string orig("For example PI is approximately ");
//   std::cout << string_append_format(&orig, "%12.6f", 3.14);
template<typename... Args>
std::string &string_append_format (std::string *dst,
                                   const format_spec<Args...> &format,
                                   const Args &... args) {
    return format_internal::append_pack(
        dst, format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// stream_format()
//
// Writes to an output stream given a format string and zero or more arguments,
// generally in a manner that is more efficient than streaming the result of
// `abel:: string_format()`. The returned object must be streamed before the full
// expression ends.
//
// Example:
//
//   std::cout << stream_format("%12.6f", 3.14);
template<typename... Args>
ABEL_MUST_USE_RESULT format_internal::stream_able stream_format (
    const format_spec<Args...> &format, const Args &... args) {
    return format_internal::stream_able(
        format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// string_printf()
//
// Writes to stdout given a format string and zero or more arguments. This
// function is functionally equivalent to `std::string_printf()` (and type-safe);
// prefer `abel::string_printf()` over `std::string_printf()`.
//
// Example:
//
//   std::string_view s = "Ulaanbaatar";
//   abel::string_printf("The capital of Mongolia is %s", s);
//
//   Outputs: "The capital of Mongolia is Ulaanbaatar"
//
template<typename... Args>
int string_printf (const format_spec<Args...> &format, const Args &... args) {
    return format_internal::abel_fprintf(
        stdout, format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// string_fprintf()
//
// Writes to a file given a format string and zero or more arguments. This
// function is functionally equivalent to `std::string_fprintf()` (and type-safe);
// prefer `abel::string_fprintf()` over `std::string_fprintf()`.
//
// Example:
//
//   std::string_view s = "Ulaanbaatar";
//   abel::string_fprintf(stdout, "The capital of Mongolia is %s", s);
//
//   Outputs: "The capital of Mongolia is Ulaanbaatar"
//
template<typename... Args>
int string_fprintf (std::FILE *output, const format_spec<Args...> &format,
                    const Args &... args) {
    return format_internal::abel_fprintf(
        output, format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// snprintf()
//
// Writes to a sized buffer given a format string and zero or more arguments.
// This function is functionally equivalent to `std::snprintf()` (and
// type-safe); prefer `abel::snprintf()` over `std::snprintf()`.
//
// In particular, a successful call to `abel::snprintf()` writes at most `size`
// bytes of the formatted output to `output`, including a NUL-terminator, and
// returns the number of bytes that would have been written if truncation did
// not occur. In the event of an error, a negative value is returned and `errno`
// is set.
//
// Example:
//
//   std::string_view s = "Ulaanbaatar";
//   char output[128];
//   abel::snprintf(output, sizeof(output),
//                  "The capital of Mongolia is %s", s);
//
//   Post-condition: output == "The capital of Mongolia is Ulaanbaatar"
//
template<typename... Args>
int string_snprintf (char *output, std::size_t size, const format_spec<Args...> &format,
                     const Args &... args) {
    return format_internal::abel_snprintf(
        output, size, format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// -----------------------------------------------------------------------------
// Custom Output Formatting Functions
// -----------------------------------------------------------------------------

// format_raw_sink
//
// format_raw_sink is a type erased wrapper around arbitrary sink objects
// specifically used as an argument to `format()`.
// format_raw_sink does not own the passed sink object. The passed object must
// outlive the format_raw_sink.
class format_raw_sink {
public:
    // Implicitly convert from any type that provides the hook function as
    // described above.
    template<typename T,
        typename = typename std::enable_if<std::is_constructible<
            format_internal::format_raw_sink_impl, T *>::value>::type>
    format_raw_sink (T *raw)  // NOLINT
        : _sink(raw) { }

private:
    friend format_internal::format_raw_sink_impl;
    format_internal::format_raw_sink_impl _sink;
};

// format()
//
// Writes a formatted string to an arbitrary sink object (implementing the
// `abel::format_raw_sink` interface), using a format string and zero or more
// additional arguments.
//
// By default, `std::string` and `std::ostream` are supported as destination
// objects. If a `std::string` is used the formatted string is appended to it.
//
// `abel::format()` is a generic version of `abel::string_format(), for custom
// sinks. The format string, like format strings for `string_format()`, is checked
// at compile-time.
//
// On failure, this function returns `false` and the state of the sink is
// unspecified.
template<typename... Args>
bool format (format_raw_sink raw_sink, const format_spec<Args...> &format,
             const Args &... args) {
    return format_internal::format_untyped(
        format_internal::format_raw_sink_impl::Extract(raw_sink),
        format_internal::untyped_format_spec_impl::Extract(format),
        {format_internal::format_arg_impl(args)...});
}

// format_arg
//
// A type-erased handle to a format argument specifically used as an argument to
// `FormatUntyped()`. You may construct `format_arg` by passing
// reference-to-const of any printable type. `format_arg` is both copyable and
// assignable. The source data must outlive the `format_arg` instance. See
// example below.
//
using format_arg = format_internal::format_arg_impl;

// FormatUntyped()
//
// Writes a formatted string to an arbitrary sink object (implementing the
// `abel::format_raw_sink` interface), using an `untyped_format_spec` and zero or
// more additional arguments.
//
// This function acts as the most generic formatting function in the
// `str_format` library. The caller provides a raw sink, an unchecked format
// string, and (usually) a runtime specified list of arguments; no compile-time
// checking of formatting is performed within this function. As a result, a
// caller should check the return value to verify that no error occurred.
// On failure, this function returns `false` and the state of the sink is
// unspecified.
//
// The arguments are provided in an `abel::Span<const abel::format_arg>`.
// Each `abel::format_arg` object binds to a single argument and keeps a
// reference to it. The values used to create the `format_arg` objects must
// outlive this function call. (See `str_format_arg.h` for information on
// the `format_arg` class.)_
//
// Example:
//
//   std::optional<std::string> FormatDynamic(
//       const std::string& in_format,
//       const vector<std::string>& in_args) {
//     std::string out;
//     std::vector<abel::format_arg> args;
//     for (const auto& v : in_args) {
//       // It is important that 'v' is a reference to the objects in in_args.
//       // The values we pass to format_arg must outlive the call to
//       // FormatUntyped.
//       args.emplace_back(v);
//     }
//     abel::untyped_format_spec format(in_format);
//     if (!abel::FormatUntyped(&out, format, args)) {
//       return std::nullopt;
//     }
//     return std::move(out);
//   }
//
ABEL_MUST_USE_RESULT ABEL_FORCE_INLINE bool FormatUntyped (
    format_raw_sink raw_sink, const untyped_format_spec &format,
    abel::Span<const format_arg> args) {
    return format_internal::format_untyped(
        format_internal::format_raw_sink_impl::Extract(raw_sink),
        format_internal::untyped_format_spec_impl::Extract(format), args);
}

}  // namespace abel

#endif  // ABEL_STRINGS_STR_FORMAT_H_
