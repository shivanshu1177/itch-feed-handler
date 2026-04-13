#include <benchmark/benchmark.h>
#include "concurrency/seqlock.hpp"
#include "orderbook/level2_book.hpp"  // BboSnapshot
#include <thread>
#include <atomic>

using namespace itch;

static void BM_SeqLockWrite(benchmark::State& state) {
    SeqLock<BboSnapshot> sl;
    BboSnapshot bbo{};
    bbo.bid_price = 10000; bbo.ask_price = 10100;

    for (auto _ : state) {
        sl.store(bbo);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SeqLockWrite)->MinTime(2.0)->Unit(benchmark::kNanosecond);

static void BM_SeqLockRead(benchmark::State& state) {
    SeqLock<BboSnapshot> sl;
    BboSnapshot bbo{};
    sl.store(bbo);

    for (auto _ : state) {
        benchmark::DoNotOptimize(sl.load());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SeqLockRead)->MinTime(2.0)->Unit(benchmark::kNanosecond);

static void BM_SeqLockContended(benchmark::State& state) {
    SeqLock<BboSnapshot> sl;
    BboSnapshot bbo{};
    std::atomic<bool> running{true};
    const int n_readers = static_cast<int>(state.range(0));

    std::vector<std::thread> readers;
    std::atomic<uint64_t> total_reads{0};
    for (int i = 0; i < n_readers; ++i) {
        readers.emplace_back([&] {
            uint64_t r = 0;
            while (running.load(std::memory_order_relaxed)) {
                benchmark::DoNotOptimize(sl.load());
                ++r;
            }
            total_reads.fetch_add(r);
        });
    }

    for (auto _ : state) {
        sl.store(bbo);
        benchmark::ClobberMemory();
    }

    running.store(false);
    for (auto& t : readers) t.join();

    state.SetItemsProcessed(state.iterations());
    state.counters["reads"] = static_cast<double>(total_reads.load());
}
BENCHMARK(BM_SeqLockContended)->Arg(1)->Arg(4)->Arg(8)
    ->MinTime(2.0)->Unit(benchmark::kNanosecond)->UseRealTime();
