//

#include <abel/random/uniform_real_distribution.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <abel/log/raw_logging.h>
#include <test/testing/chi_square.h>
#include <test/testing/distribution_test_util.h>
#include <abel/random/internal/sequence_urbg.h>
#include <abel/random/random.h>
#include <abel/strings/str_cat.h>

// NOTES:
// * Some documentation on generating random real values suggests that
//   it is possible to use std::nextafter(b, DBL_MAX) to generate a value on
//   the closed range [a, b]. Unfortunately, that technique is not universally
//   reliable due to floating point quantization.
//
// * abel::uniform_real_distribution<float> generates between 2^28 and 2^29
//   distinct floating point values in the range [0, 1).
//
// * abel::uniform_real_distribution<float> generates at least 2^23 distinct
//   floating point values in the range [1, 2). This should be the same as
//   any other range covered by a single exponent in IEEE 754.
//
// * abel::uniform_real_distribution<double> generates more than 2^52 distinct
//   values in the range [0, 1), and should generate at least 2^52 distinct
//   values in the range of [1, 2).
//

namespace {

    template<typename RealType>
    class UniformRealDistributionTest : public ::testing::Test {
    };

#if defined(__EMSCRIPTEN__)
    using RealTypes = ::testing::Types<float, double>;
#else
    using RealTypes = ::testing::Types<float, double, long double>;
#endif  // defined(__EMSCRIPTEN__)

    TYPED_TEST_SUITE(UniformRealDistributionTest, RealTypes);

    TYPED_TEST(UniformRealDistributionTest, ParamSerializeTest) {
        using param_type =
        typename abel::uniform_real_distribution<TypeParam>::param_type;

        constexpr const TypeParam a{1152921504606846976};

        constexpr int kCount = 1000;
        abel::InsecureBitGen gen;
        for (const auto &param : {
                param_type(),
                param_type(TypeParam(2.0), TypeParam(2.0)),  // Same
                param_type(TypeParam(-0.1), TypeParam(0.1)),
                param_type(TypeParam(0.05), TypeParam(0.12)),
                param_type(TypeParam(-0.05), TypeParam(0.13)),
                param_type(TypeParam(-0.05), TypeParam(-0.02)),
                // double range = 0
                // 2^60 , 2^60 + 2^6
                param_type(a, TypeParam(1152921504606847040)),
                // 2^60 , 2^60 + 2^7
                param_type(a, TypeParam(1152921504606847104)),
                // double range = 2^8
                // 2^60 , 2^60 + 2^8
                param_type(a, TypeParam(1152921504606847232)),
                // float range = 0
                // 2^60 , 2^60 + 2^36
                param_type(a, TypeParam(1152921573326323712)),
                // 2^60 , 2^60 + 2^37
                param_type(a, TypeParam(1152921642045800448)),
                // float range = 2^38
                // 2^60 , 2^60 + 2^38
                param_type(a, TypeParam(1152921779484753920)),
                // Limits
                param_type(0, std::numeric_limits<TypeParam>::max()),
                param_type(std::numeric_limits<TypeParam>::lowest(), 0),
                param_type(0, std::numeric_limits<TypeParam>::epsilon()),
                param_type(-std::numeric_limits<TypeParam>::epsilon(),
                           std::numeric_limits<TypeParam>::epsilon()),
                param_type(std::numeric_limits<TypeParam>::epsilon(),
                           2 * std::numeric_limits<TypeParam>::epsilon()),
        }) {
            // Validate parameters.
            const auto a = param.a();
            const auto b = param.b();
            abel::uniform_real_distribution<TypeParam> before(a, b);
            EXPECT_EQ(before.a(), param.a());
            EXPECT_EQ(before.b(), param.b());

            {
                abel::uniform_real_distribution<TypeParam> via_param(param);
                EXPECT_EQ(via_param, before);
            }

            std::stringstream ss;
            ss << before;
            abel::uniform_real_distribution<TypeParam> after(TypeParam(1.0),
                                                             TypeParam(3.1));

            EXPECT_NE(before.a(), after.a());
            EXPECT_NE(before.b(), after.b());
            EXPECT_NE(before.param(), after.param());
            EXPECT_NE(before, after);

            ss >> after;

            EXPECT_EQ(before.a(), after.a());
            EXPECT_EQ(before.b(), after.b());
            EXPECT_EQ(before.param(), after.param());
            EXPECT_EQ(before, after);

            // Smoke test.
            auto sample_min = after.max();
            auto sample_max = after.min();
            for (int i = 0; i < kCount; i++) {
                auto sample = after(gen);
                // Failure here indicates a bug in uniform_real_distribution::operator(),
                // or bad parameters--range too large, etc.
                if (after.min() == after.max()) {
                    EXPECT_EQ(sample, after.min());
                } else {
                    EXPECT_GE(sample, after.min());
                    EXPECT_LT(sample, after.max());
                }
                if (sample > sample_max) {
                    sample_max = sample;
                }
                if (sample < sample_min) {
                    sample_min = sample;
                }
            }

            if (!std::is_same<TypeParam, long double>::value) {
                // static_cast<double>(long double) can overflow.
                std::string msg = abel::string_cat("Range: ", static_cast<double>(sample_min),
                                                   ", ", static_cast<double>(sample_max));
                ABEL_RAW_LOG(INFO, "%s", msg.c_str());
            }
        }
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4756)  // Constant arithmetic overflow.
#endif

    TYPED_TEST(UniformRealDistributionTest, ViolatesPreconditionsDeathTest) {
#if GTEST_HAS_DEATH_TEST
        // Hi < Lo
        EXPECT_DEBUG_DEATH(
                { abel::uniform_real_distribution<TypeParam> dist(10.0, 1.0); }, "");

        // Hi - Lo > numeric_limits<>::max()
        EXPECT_DEBUG_DEATH(
                {
                    abel::uniform_real_distribution<TypeParam> dist(
                            std::numeric_limits<TypeParam>::lowest(),
                            std::numeric_limits<TypeParam>::max());
                },
                "");
#endif  // GTEST_HAS_DEATH_TEST
#if defined(NDEBUG)
        // opt-mode, for invalid parameters, will generate a garbage value,
        // but should not enter an infinite loop.
        abel::InsecureBitGen gen;
        {
          abel::uniform_real_distribution<TypeParam> dist(10.0, 1.0);
          auto x = dist(gen);
          EXPECT_FALSE(std::isnan(x)) << x;
        }
        {
          abel::uniform_real_distribution<TypeParam> dist(
              std::numeric_limits<TypeParam>::lowest(),
              std::numeric_limits<TypeParam>::max());
          auto x = dist(gen);
          // Infinite result.
          EXPECT_FALSE(std::isfinite(x)) << x;
        }
#endif  // NDEBUG
    }

#ifdef _MSC_VER
#pragma warning(pop)  // warning(disable:4756)
#endif

    TYPED_TEST(UniformRealDistributionTest, TestMoments) {
        constexpr int kSize = 1000000;
        std::vector<double> values(kSize);

        abel::InsecureBitGen rng;
        abel::uniform_real_distribution<TypeParam> dist;
        for (int i = 0; i < kSize; i++) {
            values[i] = dist(rng);
        }

        const auto moments =
                abel::random_internal::ComputeDistributionMoments(values);
        EXPECT_NEAR(0.5, moments.mean, 0.01);
        EXPECT_NEAR(1 / 12.0, moments.variance, 0.015);
        EXPECT_NEAR(0.0, moments.skewness, 0.02);
        EXPECT_NEAR(9 / 5.0, moments.kurtosis, 0.015);
    }

    TYPED_TEST(UniformRealDistributionTest, ChiSquaredTest50) {
        using abel::random_internal::kChiSquared;
        using param_type =
        typename abel::uniform_real_distribution<TypeParam>::param_type;

        constexpr size_t kTrials = 100000;
        constexpr int kBuckets = 50;
        constexpr double kExpected =
                static_cast<double>(kTrials) / static_cast<double>(kBuckets);

        // 1-in-100000 threshold, but remember, there are about 8 tests
        // in this file. And the test could fail for other reasons.
        // Empirically validated with --runs_per_test=10000.
        const int kThreshold =
                abel::random_internal::ChiSquareValue(kBuckets - 1, 0.999999);

        abel::InsecureBitGen rng;
        for (const auto &param : {param_type(0, 1), param_type(5, 12),
                                  param_type(-5, 13), param_type(-5, -2)}) {
            const double min_val = param.a();
            const double max_val = param.b();
            const double factor = kBuckets / (max_val - min_val);

            std::vector<int32_t> counts(kBuckets, 0);
            abel::uniform_real_distribution<TypeParam> dist(param);
            for (size_t i = 0; i < kTrials; i++) {
                auto x = dist(rng);
                auto bucket = static_cast<size_t>((x - min_val) * factor);
                counts[bucket]++;
            }

            double chi_square = abel::random_internal::ChiSquareWithExpected(
                    std::begin(counts), std::end(counts), kExpected);
            if (chi_square > kThreshold) {
                double p_value =
                        abel::random_internal::ChiSquarePValue(chi_square, kBuckets);

                // Chi-squared test failed. Output does not appear to be uniform.
                std::string msg;
                for (const auto &a : counts) {
                    abel::string_append(&msg, a, "\n");
                }
                abel::string_append(&msg, kChiSquared, " p-value ", p_value, "\n");
                abel::string_append(&msg, "High ", kChiSquared, " value: ", chi_square, " > ",
                                    kThreshold);
                ABEL_RAW_LOG(INFO, "%s", msg.c_str());
                FAIL() << msg;
            }
        }
    }

    TYPED_TEST(UniformRealDistributionTest, StabilityTest) {
        // abel::uniform_real_distribution stability relies only on
        // random_internal::RandU64ToDouble and random_internal::RandU64ToFloat.
        abel::random_internal::sequence_urbg urbg(
                {0x0003eb76f6f7f755ull, 0xFFCEA50FDB2F953Bull, 0xC332DDEFBE6C5AA5ull,
                 0x6558218568AB9702ull, 0x2AEF7DAD5B6E2F84ull, 0x1521B62829076170ull,
                 0xECDD4775619F1510ull, 0x13CCA830EB61BD96ull, 0x0334FE1EAA0363CFull,
                 0xB5735C904C70A239ull, 0xD59E9E0BCBAADE14ull, 0xEECC86BC60622CA7ull});

        std::vector<int> output(12);

        abel::uniform_real_distribution<TypeParam> dist;
        std::generate(std::begin(output), std::end(output), [&] {
            return static_cast<int>(TypeParam(1000000) * dist(urbg));
        });

        EXPECT_THAT(
                output,  //
                testing::ElementsAre(59, 999246, 762494, 395876, 167716, 82545, 925251,
                                     77341, 12527, 708791, 834451, 932808));
    }

    TEST(UniformRealDistributionTest, AlgorithmBounds) {
        abel::uniform_real_distribution<double> dist;

        {
            // This returns the smallest value >0 from abel::uniform_real_distribution.
            abel::random_internal::sequence_urbg urbg({0x0000000000000001ull});
            double a = dist(urbg);
            EXPECT_EQ(a, 5.42101086242752217004e-20);
        }

        {
            // This returns a value very near 0.5 from abel::uniform_real_distribution.
            abel::random_internal::sequence_urbg urbg({0x7fffffffffffffefull});
            double a = dist(urbg);
            EXPECT_EQ(a, 0.499999999999999944489);
        }
        {
            // This returns a value very near 0.5 from abel::uniform_real_distribution.
            abel::random_internal::sequence_urbg urbg({0x8000000000000000ull});
            double a = dist(urbg);
            EXPECT_EQ(a, 0.5);
        }

        {
            // This returns the largest value <1 from abel::uniform_real_distribution.
            abel::random_internal::sequence_urbg urbg({0xFFFFFFFFFFFFFFEFull});
            double a = dist(urbg);
            EXPECT_EQ(a, 0.999999999999999888978);
        }
        {
            // This *ALSO* returns the largest value <1.
            abel::random_internal::sequence_urbg urbg({0xFFFFFFFFFFFFFFFFull});
            double a = dist(urbg);
            EXPECT_EQ(a, 0.999999999999999888978);
        }
    }

}  // namespace
