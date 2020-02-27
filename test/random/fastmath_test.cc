//

#include <abel/random/internal/fastmath.h>

#include <gtest/gtest.h>

#if defined(__native_client__) || defined(__EMSCRIPTEN__)
// NACL has a less accurate implementation of std::log2 than most of
// the other platforms. For some values which should have integral results,
// sometimes NACL returns slightly larger values.
//
// The MUSL libc used by emscripten also has a similar bug.
#define ABEL_RANDOM_INACCURATE_LOG2
#endif

namespace {

    TEST(DistributionImplTest, LeadingSetBit) {
        using abel::random_internal::LeadingSetBit;
        constexpr uint64_t kZero = 0;
        EXPECT_EQ(0, LeadingSetBit(kZero));
        EXPECT_EQ(64, LeadingSetBit(~kZero));

        for (int index = 0; index < 64; index++) {
            uint64_t x = static_cast<uint64_t>(1) << index;
            EXPECT_EQ(index + 1, LeadingSetBit(x)) << index;
            EXPECT_EQ(index + 1, LeadingSetBit(x + x - 1)) << index;
        }
    }

    TEST(FastMathTest, IntLog2FloorTest) {
        using abel::random_internal::IntLog2Floor;
        constexpr uint64_t kZero = 0;
        EXPECT_EQ(0, IntLog2Floor(0));  // boundary. return 0.
        EXPECT_EQ(0, IntLog2Floor(1));
        EXPECT_EQ(1, IntLog2Floor(2));
        EXPECT_EQ(63, IntLog2Floor(~kZero));

        // A boundary case: Converting 0xffffffffffffffff requires > 53
        // bits of precision, so the conversion to double rounds up,
        // and the result of std::log2(x) > IntLog2Floor(x).
        EXPECT_LT(IntLog2Floor(~kZero), static_cast<int>(std::log2(~kZero)));

        for (int i = 0; i < 64; i++) {
            const uint64_t i_pow_2 = static_cast<uint64_t>(1) << i;
            EXPECT_EQ(i, IntLog2Floor(i_pow_2));
            EXPECT_EQ(i, static_cast<int>(std::log2(i_pow_2)));

            uint64_t y = i_pow_2;
            for (int j = i - 1; j > 0; --j) {
                y = y | (i_pow_2 >> j);
                EXPECT_EQ(i, IntLog2Floor(y));
            }
        }
    }

    TEST(FastMathTest, IntLog2CeilTest) {
        using abel::random_internal::IntLog2Ceil;
        constexpr uint64_t kZero = 0;
        EXPECT_EQ(0, IntLog2Ceil(0));  // boundary. return 0.
        EXPECT_EQ(0, IntLog2Ceil(1));
        EXPECT_EQ(1, IntLog2Ceil(2));
        EXPECT_EQ(64, IntLog2Ceil(~kZero));

        // A boundary case: Converting 0xffffffffffffffff requires > 53
        // bits of precision, so the conversion to double rounds up,
        // and the result of std::log2(x) > IntLog2Floor(x).
        EXPECT_LE(IntLog2Ceil(~kZero), static_cast<int>(std::log2(~kZero)));

        for (int i = 0; i < 64; i++) {
            const uint64_t i_pow_2 = static_cast<uint64_t>(1) << i;
            EXPECT_EQ(i, IntLog2Ceil(i_pow_2));
#ifndef ABEL_RANDOM_INACCURATE_LOG2
            EXPECT_EQ(i, static_cast<int>(std::ceil(std::log2(i_pow_2))));
#endif

            uint64_t y = i_pow_2;
            for (int j = i - 1; j > 0; --j) {
                y = y | (i_pow_2 >> j);
                EXPECT_EQ(i + 1, IntLog2Ceil(y));
            }
        }
    }

    TEST(FastMathTest, StirlingLogFactorial) {
        using abel::random_internal::StirlingLogFactorial;

        EXPECT_NEAR(StirlingLogFactorial(1.0), 0, 1e-3);
        EXPECT_NEAR(StirlingLogFactorial(1.50), 0.284683, 1e-3);
        EXPECT_NEAR(StirlingLogFactorial(2.0), 0.69314718056, 1e-4);

        for (int i = 2; i < 50; i++) {
            double d = static_cast<double>(i);
            EXPECT_NEAR(StirlingLogFactorial(d), std::lgamma(d + 1), 3e-5);
        }
    }

}  // namespace
