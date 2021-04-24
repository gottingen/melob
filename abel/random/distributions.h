//
// -----------------------------------------------------------------------------
// File: distributions.h
// -----------------------------------------------------------------------------
//
// This header defines functions representing distributions, which you use in
// combination with an abel random bit generator to produce random values
// according to the rules of that distribution.
//
// The abel random library defines the following distributions within this
// file:
//
//   * `abel::uniform` for uniform (constant) distributions having constant
//     probability
//   * `abel::Bernoulli` for discrete distributions having exactly two outcomes
//   * `abel::Beta` for continuous distributions parameterized through two
//     free parameters
//   * `abel::Exponential` for discrete distributions of events occurring
//     continuously and independently at a constant average rate
//   * `abel::Gaussian` (also known as "normal distributions") for continuous
//     distributions using an associated quadratic function
//   * `abel::LogUniform` for continuous uniform distributions where the log
//     to the given base of all values is uniform
//   * `abel::Poisson` for discrete probability distributions that express the
//     probability of a given number of events occurring within a fixed interval
//   * `abel::Zipf` for discrete probability distributions commonly used for
//     modelling of rare events
//
// Prefer use of these distribution function classes over manual construction of
// your own distribution classes, as it allows library maintainers greater
// flexibility to change the underlying implementation in the future.

#ifndef ABEL_RANDOM_DISTRIBUTIONS_H_
#define ABEL_RANDOM_DISTRIBUTIONS_H_

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <type_traits>

#include "abel/utility/internal/inline_variable.h"
#include "abel/random/bernoulli_distribution.h"
#include "abel/random/beta_distribution.h"
#include "abel/random/distribution_format_traits.h"
#include "abel/random/exponential_distribution.h"
#include "abel/random/gaussian_distribution.h"
#include "abel/random/internal/distributions.h"  // IWYU pragma: export
#include "abel/random/internal/uniform_helper.h"  // IWYU pragma: export
#include "abel/random/log_uniform_int_distribution.h"
#include "abel/random/poisson_distribution.h"
#include "abel/random/uniform_int_distribution.h"
#include "abel/random/uniform_real_distribution.h"
#include "abel/random/zipf_distribution.h"

namespace abel {


ABEL_INTERNAL_INLINE_CONSTEXPR(interval_closed_closed_tag, IntervalClosedClosed,
                               {});
ABEL_INTERNAL_INLINE_CONSTEXPR(interval_closed_closed_tag, IntervalClosed, {});
ABEL_INTERNAL_INLINE_CONSTEXPR(interval_closed_open_tag, IntervalClosedOpen, {});
ABEL_INTERNAL_INLINE_CONSTEXPR(interval_open_open_tag, IntervalOpenOpen, {});
ABEL_INTERNAL_INLINE_CONSTEXPR(interval_open_open_tag, IntervalOpen, {});
ABEL_INTERNAL_INLINE_CONSTEXPR(interval_open_closed_tag, IntervalOpenClosed, {});

// -----------------------------------------------------------------------------
// abel::uniform<T>(tag, bitgen, lo, hi)
// -----------------------------------------------------------------------------
//
// `abel::uniform()` produces random values of type `T` uniformly distributed in
// a defined interval {lo, hi}. The interval `tag` defines the type of interval
// which should be one of the following possible values:
//
//   * `abel::IntervalOpenOpen`
//   * `abel::IntervalOpenClosed`
//   * `abel::IntervalClosedOpen`
//   * `abel::IntervalClosedClosed`
//
// where "open" refers to an exclusive value (excluded) from the output, while
// "closed" refers to an inclusive value (included) from the output.
//
// In the absence of an explicit return type `T`, `abel::uniform()` will deduce
// the return type based on the provided endpoint arguments {A lo, B hi}.
// Given these endpoints, one of {A, B} will be chosen as the return type, if
// a type can be implicitly converted into the other in a lossless way. The
// lack of any such implicit conversion between {A, B} will produce a
// compile-time error
//
// See https://en.wikipedia.org/wiki/Uniform_distribution_(continuous)
//
// Example:
//
//   abel::bit_gen bitgen;
//
//   // Produce a random float value between 0.0 and 1.0, inclusive
//   auto x = abel::uniform(abel::IntervalClosedClosed, bitgen, 0.0f, 1.0f);
//
//   // The most common interval of `abel::IntervalClosedOpen` is available by
//   // default:
//
//   auto x = abel::uniform(bitgen, 0.0f, 1.0f);
//
//   // Return-types are typically inferred from the arguments, however callers
//   // can optionally provide an explicit return-type to the template.
//
//   auto x = abel::uniform<float>(bitgen, 0, 1);
//
template<typename R = void, typename TagType, typename URBG>
typename abel::enable_if_t<!std::is_same<R, void>::value, R>  //
uniform(TagType tag,
        URBG &&urbg,  // NOLINT(runtime/references)
        R lo, R hi) {
    using gen_t = abel::decay_t<URBG>;
    using distribution_t = random_internal::uniform_distribution_wrapper<R>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    auto a = random_internal::uniform_lower_bound(tag, lo, hi);
    auto b = random_internal::uniform_upper_bound(tag, lo, hi);
    if (a > b) return a;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, tag, lo, hi);
}

// abel::uniform<T>(bitgen, lo, hi)
//
// Overload of `uniform()` using the default closed-open interval of [lo, hi),
// and returning values of type `T`
template<typename R = void, typename URBG>
typename abel::enable_if_t<!std::is_same<R, void>::value, R>  //
uniform(URBG &&urbg,  // NOLINT(runtime/references)
        R lo, R hi) {
    using gen_t = abel::decay_t<URBG>;
    using distribution_t = random_internal::uniform_distribution_wrapper<R>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    constexpr auto tag = abel::IntervalClosedOpen;
    auto a = random_internal::uniform_lower_bound(tag, lo, hi);
    auto b = random_internal::uniform_upper_bound(tag, lo, hi);
    if (a > b) return a;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, lo, hi);
}

// abel::uniform(tag, bitgen, lo, hi)
//
// Overload of `uniform()` using different (but compatible) lo, hi types. Note
// that a compile-error will result if the return type cannot be deduced
// correctly from the passed types.
template<typename R = void, typename TagType, typename URBG, typename A,
        typename B>
typename abel::enable_if_t<std::is_same<R, void>::value,
        random_internal::uniform_inferred_return_t<A, B>>
uniform(TagType tag,
        URBG &&urbg,  // NOLINT(runtime/references)
        A lo, B hi) {
    using gen_t = abel::decay_t<URBG>;
    using return_t = typename random_internal::uniform_inferred_return_t<A, B>;
    using distribution_t = random_internal::uniform_distribution_wrapper<return_t>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    auto a = random_internal::uniform_lower_bound<return_t>(tag, lo, hi);
    auto b = random_internal::uniform_upper_bound<return_t>(tag, lo, hi);
    if (a > b) return a;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, tag, static_cast<return_t>(lo),
                                      static_cast<return_t>(hi));
}

// abel::uniform(bitgen, lo, hi)
//
// Overload of `uniform()` using different (but compatible) lo, hi types and the
// default closed-open interval of [lo, hi). Note that a compile-error will
// result if the return type cannot be deduced correctly from the passed types.
template<typename R = void, typename URBG, typename A, typename B>
typename abel::enable_if_t<std::is_same<R, void>::value,
        random_internal::uniform_inferred_return_t<A, B>>
uniform(URBG &&urbg,  // NOLINT(runtime/references)
        A lo, B hi) {
    using gen_t = abel::decay_t<URBG>;
    using return_t = typename random_internal::uniform_inferred_return_t<A, B>;
    using distribution_t = random_internal::uniform_distribution_wrapper<return_t>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    constexpr auto tag = abel::IntervalClosedOpen;
    auto a = random_internal::uniform_lower_bound<return_t>(tag, lo, hi);
    auto b = random_internal::uniform_upper_bound<return_t>(tag, lo, hi);
    if (a > b) return a;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, static_cast<return_t>(lo),
                                      static_cast<return_t>(hi));
}

// abel::uniform<unsigned T>(bitgen)
//
// Overload of uniform() using the minimum and maximum values of a given type
// `T` (which must be unsigned), returning a value of type `unsigned T`
template<typename R, typename URBG>
typename abel::enable_if_t<!std::is_signed<R>::value, R>  //
uniform(URBG &&urbg) {  // NOLINT(runtime/references)
    using gen_t = abel::decay_t<URBG>;
    using distribution_t = random_internal::uniform_distribution_wrapper<R>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg);
}

// -----------------------------------------------------------------------------
// abel::Bernoulli(bitgen, p)
// -----------------------------------------------------------------------------
//
// `abel::Bernoulli` produces a random boolean value, with probability `p`
// (where 0.0 <= p <= 1.0) equaling `true`.
//
// Prefer `abel::Bernoulli` to produce boolean values over other alternatives
// such as comparing an `abel::uniform()` value to a specific output.
//
// See https://en.wikipedia.org/wiki/Bernoulli_distribution
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   if (abel::Bernoulli(bitgen, 1.0/3721.0)) {
//     std::cout << "Asteroid field navigation successful.";
//   }
//
template<typename URBG>
bool Bernoulli(URBG &&urbg,  // NOLINT(runtime/references)
               double p) {
    using gen_t = abel::decay_t<URBG>;
    using distribution_t = abel::bernoulli_distribution;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, p);
}

// -----------------------------------------------------------------------------
// abel::Beta<T>(bitgen, alpha, beta)
// -----------------------------------------------------------------------------
//
// `abel::Beta` produces a floating point number distributed in the closed
// interval [0,1] and parameterized by two values `alpha` and `beta` as per a
// Beta distribution. `T` must be a floating point type, but may be inferred
// from the types of `alpha` and `beta`.
//
// See https://en.wikipedia.org/wiki/Beta_distribution.
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   double sample = abel::Beta(bitgen, 3.0, 2.0);
//
template<typename RealType, typename URBG>
RealType Beta(URBG &&urbg,  // NOLINT(runtime/references)
              RealType alpha, RealType beta) {
    static_assert(
            std::is_floating_point<RealType>::value,
            "Template-argument 'RealType' must be a floating-point type, in "
            "abel::Beta<RealType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::beta_distribution<RealType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, alpha, beta);
}

// -----------------------------------------------------------------------------
// abel::Exponential<T>(bitgen, lambda = 1)
// -----------------------------------------------------------------------------
//
// `abel::Exponential` produces a floating point number for discrete
// distributions of events occurring continuously and independently at a
// constant average rate. `T` must be a floating point type, but may be inferred
// from the type of `lambda`.
//
// See https://en.wikipedia.org/wiki/Exponential_distribution.
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   double call_length = abel::Exponential(bitgen, 7.0);
//
template<typename RealType, typename URBG>
RealType Exponential(URBG &&urbg,  // NOLINT(runtime/references)
                     RealType lambda = 1) {
    static_assert(
            std::is_floating_point<RealType>::value,
            "Template-argument 'RealType' must be a floating-point type, in "
            "abel::Exponential<RealType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::exponential_distribution<RealType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, lambda);
}

// -----------------------------------------------------------------------------
// abel::Gaussian<T>(bitgen, mean = 0, stddev = 1)
// -----------------------------------------------------------------------------
//
// `abel::Gaussian` produces a floating point number selected from the Gaussian
// (ie. "Normal") distribution. `T` must be a floating point type, but may be
// inferred from the types of `mean` and `stddev`.
//
// See https://en.wikipedia.org/wiki/Normal_distribution
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   double giraffe_height = abel::Gaussian(bitgen, 16.3, 3.3);
//
template<typename RealType, typename URBG>
RealType Gaussian(URBG &&urbg,  // NOLINT(runtime/references)
                  RealType mean = 0, RealType stddev = 1) {
    static_assert(
            std::is_floating_point<RealType>::value,
            "Template-argument 'RealType' must be a floating-point type, in "
            "abel::Gaussian<RealType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::gaussian_distribution<RealType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, mean, stddev);
}

// -----------------------------------------------------------------------------
// abel::LogUniform<T>(bitgen, lo, hi, base = 2)
// -----------------------------------------------------------------------------
//
// `abel::LogUniform` produces random values distributed where the log to a
// given base of all values is uniform in a closed interval [lo, hi]. `T` must
// be an integral type, but may be inferred from the types of `lo` and `hi`.
//
// I.e., `LogUniform(0, n, b)` is uniformly distributed across buckets
// [0], [1, b-1], [b, b^2-1] .. [b^(k-1), (b^k)-1] .. [b^floor(log(n, b)), n]
// and is uniformly distributed within each bucket.
//
// The resulting probability density is inversely related to bucket size, though
// values in the final bucket may be more likely than previous values. (In the
// extreme case where n = b^i the final value will be tied with zero as the most
// probable result.
//
// If `lo` is nonzero then this distribution is shifted to the desired interval,
// so LogUniform(lo, hi, b) is equivalent to LogUniform(0, hi-lo, b)+lo.
//
// See http://ecolego.facilia.se/ecolego/show/Log-uniform%20Distribution
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   int v = abel::LogUniform(bitgen, 0, 1000);
//
template<typename IntType, typename URBG>
IntType LogUniform(URBG &&urbg,  // NOLINT(runtime/references)
                   IntType lo, IntType hi, IntType base = 2) {
    static_assert(std::is_integral<IntType>::value,
                  "Template-argument 'IntType' must be an integral type, in "
                  "abel::LogUniform<IntType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::log_uniform_int_distribution<IntType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, lo, hi, base);
}

// -----------------------------------------------------------------------------
// abel::Poisson<T>(bitgen, mean = 1)
// -----------------------------------------------------------------------------
//
// `abel::Poisson` produces discrete probabilities for a given number of events
// occurring within a fixed interval within the closed interval [0, max]. `T`
// must be an integral type.
//
// See https://en.wikipedia.org/wiki/Poisson_distribution
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   int requests_per_minute = abel::Poisson<int>(bitgen, 3.2);
//
template<typename IntType, typename URBG>
IntType Poisson(URBG &&urbg,  // NOLINT(runtime/references)
                double mean = 1.0) {
    static_assert(std::is_integral<IntType>::value,
                  "Template-argument 'IntType' must be an integral type, in "
                  "abel::Poisson<IntType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::poisson_distribution<IntType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, mean);
}

// -----------------------------------------------------------------------------
// abel::Zipf<T>(bitgen, hi = max, q = 2, v = 1)
// -----------------------------------------------------------------------------
//
// `abel::Zipf` produces discrete probabilities commonly used for modelling of
// rare events over the closed interval [0, hi]. The parameters `v` and `q`
// determine the skew of the distribution. `T`  must be an integral type, but
// may be inferred from the type of `hi`.
//
// See http://mathworld.wolfram.com/ZipfDistribution.html
//
// Example:
//
//   abel::bit_gen bitgen;
//   ...
//   int term_rank = abel::Zipf<int>(bitgen);
//
template<typename IntType, typename URBG>
IntType Zipf(URBG &&urbg,  // NOLINT(runtime/references)
             IntType hi = (std::numeric_limits<IntType>::max)(), double q = 2.0,
             double v = 1.0) {
    static_assert(std::is_integral<IntType>::value,
                  "Template-argument 'IntType' must be an integral type, in "
                  "abel::Zipf<IntType, URBG>(...)");

    using gen_t = abel::decay_t<URBG>;
    using distribution_t = typename abel::zipf_distribution<IntType>;
    using format_t = random_internal::distribution_format_traits<distribution_t>;

    return random_internal::distribution_caller<gen_t>::template call<
            distribution_t, format_t>(&urbg, hi, q, v);
}


}  // namespace abel

#endif  // ABEL_RANDOM_DISTRIBUTIONS_H_
