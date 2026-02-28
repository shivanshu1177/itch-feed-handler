#include <gtest/gtest.h>
#include "concurrency/seqlock.hpp"
#include <thread>
#include <vector>

using namespace itch;

TEST(SeqLock, SingleWriteRead) {
    SeqLock<int> sl;
    sl.store(42);
    EXPECT_EQ(sl.load(), 42);
}

TEST(SeqLock, MultipleWrites) {
    SeqLock<int> sl;
    for (int i = 0; i < 1000; ++i) {
        sl.store(i);
        EXPECT_EQ(sl.load(), i);
    }
}

TEST(SeqLock, ConcurrentReadWrite) {
    struct Data { int a; int b; };
    SeqLock<Data> sl;
    sl.store({0, 0});

    constexpr int ITERS = 100000;
    std::atomic<bool> done{false};

    std::thread writer([&] {
        for (int i = 0; i < ITERS; ++i)
            sl.store({i, i});
        done.store(true);
    });

    // Readers verify a==b (no torn reads)
    int torn_count = 0;
    while (!done.load()) {
        Data d = sl.load();
        if (d.a != d.b) ++torn_count;
    }
    writer.join();
    EXPECT_EQ(torn_count, 0) << "Torn reads detected in SeqLock";
}
