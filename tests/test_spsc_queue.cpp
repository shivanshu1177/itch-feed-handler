#include <gtest/gtest.h>
#include "concurrency/spsc_queue.hpp"
#include <thread>

using namespace itch;

TEST(SpscQueue, BasicPushPop) {
    SpscQueue<int, 16> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    int v = 0;
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, FullCapacity) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5)); // full
    int v = 0;
    EXPECT_TRUE(q.try_pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_push(5)); // now has room
}

TEST(SpscQueue, Throughput) {
    constexpr std::size_t N = 1 << 18;
    SpscQueue<uint64_t, 1 << 16> q;
    uint64_t sum_produced = 0, sum_consumed = 0;

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i) {
            while (!q.try_push(i)) {}
            sum_produced += i;
        }
    });
    std::thread consumer([&] {
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t v = 0;
            while (!q.try_pop(v)) {}
            sum_consumed += v;
        }
    });
    producer.join();
    consumer.join();
    EXPECT_EQ(sum_produced, sum_consumed);
}
