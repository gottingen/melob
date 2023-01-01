
/****************************************************************
 * Copyright (c) 2022, liyinbin
 * All rights reserved.
 * Author by liyinbin (jeff.li) lijippy@163.com
 *****************************************************************/

#include "testing/gtest_wrap.h"
#include "melon/metrics/detail/percentile.h"
#include "melon/log/logging.h"
#include <fstream>

class PercentileTest : public testing::Test {
protected:

    void SetUp() {}

    void TearDown() {}
};

TEST_F(PercentileTest, add) {
    melon::metrics_detail::percentile p;
    for (int j = 0; j < 10; ++j) {
        for (int i = 0; i < 10000; ++i) {
            p << (i + 1);
        }
        melon::metrics_detail::global_percentile_samples b = p.reset();
        uint32_t last_value = 0;
        for (uint32_t k = 1; k <= 10u; ++k) {
            uint32_t value = b.get_number(k / 10.0);
            EXPECT_GE(value, last_value);
            last_value = value;
            EXPECT_GT(value, (k * 1000 - 500)) << "k=" << k;
            EXPECT_LT(value, (k * 1000 + 500)) << "k=" << k;
        }
        MELON_LOG(INFO) << "99%:" << b.get_number(0.99) << ' '
                        << "99.9%:" << b.get_number(0.999) << ' '
                        << "99.99%:" << b.get_number(0.9999);

        std::ofstream out("out.txt");
        b.describe(out);
    }
}

TEST_F(PercentileTest, merge1) {
    // Merge 2 PercentileIntervals b1 and b2. b2 has double SAMPLE_SIZE
    // and num_added. Remaining samples of b1 and b2 in merged result should
    // be 1:2 approximately.
    const size_t N = 1000;
    const size_t SAMPLE_SIZE = 32;
    size_t belong_to_b1 = 0;
    size_t belong_to_b2 = 0;

    for (int repeat = 0; repeat < 100; ++repeat) {
        melon::metrics_detail::percentile_interval<SAMPLE_SIZE * 3> b0;
        melon::metrics_detail::percentile_interval<SAMPLE_SIZE> b1;
        for (size_t i = 0; i < N; ++i) {
            if (b1.full()) {
                b0.merge(b1);
                b1.clear();
            }
            ASSERT_TRUE(b1.add32(i));
        }
        b0.merge(b1);
        melon::metrics_detail::percentile_interval<SAMPLE_SIZE * 2> b2;
        for (size_t i = 0; i < N * 2; ++i) {
            if (b2.full()) {
                b0.merge(b2);
                b2.clear();
            }
            ASSERT_TRUE(b2.add32(i + N));
        }
        b0.merge(b2);
        for (size_t i = 0; i < b0._num_samples; ++i) {
            if (b0._samples[i] < N) {
                ++belong_to_b1;
            } else {
                ++belong_to_b2;
            }
        }
    }
    EXPECT_LT(fabs(belong_to_b1 / (double) belong_to_b2 - 0.5),
              0.2) << "belong_to_b1=" << belong_to_b1
                   << " belong_to_b2=" << belong_to_b2;
}

TEST_F(PercentileTest, merge2) {
    // Merge 2 PercentileIntervals b1 and b2 with same SAMPLE_SIZE. Add N1
    // samples to b1 and add N2 samples to b2, Remaining samples of b1 and
    // b2 in merged result should be N1:N2 approximately.

    const size_t N1 = 1000;
    const size_t N2 = 400;
    size_t belong_to_b1 = 0;
    size_t belong_to_b2 = 0;

    for (int repeat = 0; repeat < 100; ++repeat) {
        melon::metrics_detail::percentile_interval<64> b0;
        melon::metrics_detail::percentile_interval<64> b1;
        for (size_t i = 0; i < N1; ++i) {
            if (b1.full()) {
                b0.merge(b1);
                b1.clear();
            }
            ASSERT_TRUE(b1.add32(i));
        }
        b0.merge(b1);
        melon::metrics_detail::percentile_interval<64> b2;
        for (size_t i = 0; i < N2; ++i) {
            if (b2.full()) {
                b0.merge(b2);
                b2.clear();
            }
            ASSERT_TRUE(b2.add32(i + N1));
        }
        b0.merge(b2);
        for (size_t i = 0; i < b0._num_samples; ++i) {
            if (b0._samples[i] < N1) {
                ++belong_to_b1;
            } else {
                ++belong_to_b2;
            }
        }
    }
    EXPECT_LT(fabs(belong_to_b1 / (double) belong_to_b2 - N1 / (double) N2),
              0.2) << "belong_to_b1=" << belong_to_b1
                   << " belong_to_b2=" << belong_to_b2;
}

TEST_F(PercentileTest, combine_of) {
    // Combine multiple percentle samplers into one
    const int num_samplers = 10;
    // Define a base time to make all samples are in the same interval
    const uint32_t base = (1 << 30) + 1;

    const int N = 1000;
    size_t belongs[num_samplers] = {0};
    size_t total = 0;
    for (int repeat = 0; repeat < 100; ++repeat) {
        melon::metrics_detail::percentile p[num_samplers];
        for (int i = 0; i < num_samplers; ++i) {
            for (int j = 0; j < N * (i + 1); ++j) {
                p[i] << base + i * (i + 1) * N / 2 + j;
            }
        }
        std::vector<melon::metrics_detail::global_percentile_samples> result;
        result.reserve(num_samplers);
        for (int i = 0; i < num_samplers; ++i) {
            result.push_back(p[i].get_value());
        }
        melon::metrics_detail::percentile_samples<510> g;
        g.combine_of(result.begin(), result.end());
        for (size_t i = 0; i < melon::metrics_detail::NUM_INTERVALS; ++i) {
            if (g._intervals[i] == nullptr) {
                continue;
            }
            melon::metrics_detail::percentile_interval<510> &p = *g._intervals[i];
            total += p._num_samples;
            for (size_t j = 0; j < p._num_samples; ++j) {
                for (int k = 0; k < num_samplers; ++k) {
                    if ((p._samples[j] - base) / N < (k + 1) * (k + 2) / 2u) {
                        belongs[k]++;
                        break;
                    }
                }
            }
        }
    }
    for (int i = 0; i < num_samplers; ++i) {
        double expect_ratio = (double) (i + 1) * 2 /
                              (num_samplers * (num_samplers + 1));
        double actual_ratio = (double) (belongs[i]) / total;
        EXPECT_LT(fabs(expect_ratio / actual_ratio) - 1, 0.2)
                            << "expect_ratio=" << expect_ratio
                            << " actual_ratio=" << actual_ratio;

    }
}
