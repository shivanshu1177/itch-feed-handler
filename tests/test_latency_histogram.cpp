#include <gtest/gtest.h>
#include "monitoring/latency_histogram.hpp"
#include <thread>
#include <vector>

using namespace itch;

TEST(LatencyHistogram, RecordAndPercentile) {
    LatencyHistogram h;
    // Record 100 observations: 50 at 100ns, 50 at 1000ns
    for (int i = 0; i < 50; ++i) h.record(100);
    for (int i = 0; i < 50; ++i) h.record(1000);

    EXPECT_EQ(h.count(), 100u);
    EXPECT_LE(h.p50(), 1000u);   // p50 is somewhere around the median
    EXPECT_EQ(h.max(), 1000u);
}

TEST(LatencyHistogram, LinearBuckets) {
    LatencyHistogram h;
    for (uint64_t i = 0; i < 256; ++i)
        h.record(i);
    // All values in linear range
    EXPECT_EQ(h.count(), 256u);
    EXPECT_EQ(h.max(), 255u);
}

TEST(LatencyHistogram, Reset) {
    LatencyHistogram h;
    h.record(100);
    h.reset();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.max(), 0u);
}

TEST(LatencyHistogram, ConcurrentRecord) {
    LatencyHistogram h;
    constexpr int N = 10000;
    constexpr int THREADS = 4;
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t) {
        ts.emplace_back([&h] {
            for (int i = 0; i < N; ++i)
                h.record(i % 1000);
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(h.count(), static_cast<uint64_t>(N * THREADS));
}
