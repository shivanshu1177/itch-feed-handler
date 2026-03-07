#include <gtest/gtest.h>
#include "orderbook/bbo_publisher.hpp"
#include <thread>
#include <atomic>

using namespace itch;

TEST(BboPublisher, PublishRead) {
    BboPublisher pub(100);
    BboSnapshot bbo{};
    bbo.locate    = 5;
    bbo.bid_price = 10000;
    bbo.bid_qty   = 100;
    bbo.ask_price = 10100;
    bbo.ask_qty   = 200;

    pub.publish(5, bbo);
    auto read = pub.read(5);
    EXPECT_EQ(read.bid_price, 10000);
    EXPECT_EQ(read.bid_qty,   100u);
    EXPECT_EQ(read.ask_price, 10100);
    EXPECT_EQ(read.ask_qty,   200u);
}

TEST(BboPublisher, OutOfBoundsSafe) {
    BboPublisher pub(10);
    // locate >= capacity should return default (zeros)
    auto bbo = pub.read(99);
    EXPECT_EQ(bbo.bid_price, 0);
}

TEST(BboPublisher, ConcurrentReadWrite) {
    // 1 writer, 4 readers — no torn reads
    BboPublisher pub(10);
    constexpr int ITERS = 100000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 0; i < ITERS; ++i) {
            BboSnapshot b{};
            b.locate    = 1;
            b.bid_price = i;
            b.bid_qty   = static_cast<Qty>(i);
            b.ask_price = i + 100;
            b.ask_qty   = static_cast<Qty>(i + 100);
            pub.publish(1, b);
        }
        done.store(true);
    });

    int torn = 0;
    while (!done.load()) {
        auto b = pub.read(1);
        // bid_price and bid_qty should match (both set to i)
        if (b.bid_price != static_cast<Price>(b.bid_qty))
            ++torn;
    }
    writer.join();
    EXPECT_EQ(torn, 0) << "Torn reads in BboPublisher";
}
