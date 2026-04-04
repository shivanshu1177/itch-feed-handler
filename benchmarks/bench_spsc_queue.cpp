#include <benchmark/benchmark.h>
#include "concurrency/spsc_queue.hpp"
#include "protocol/itch_messages.hpp"
#include <thread>
#include <atomic>

using namespace itch;

// Single-threaded push+pop roundtrip (measures cache effects)
static void BM_SpscPushPop_uint16(benchmark::State& state) {
    SpscQueue<uint16_t, 65536> q;
    for (auto _ : state) {
        q.try_push(42);
        uint16_t v;
        benchmark::DoNotOptimize(q.try_pop(v));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop_uint16)->MinTime(2.0)->Unit(benchmark::kNanosecond);

static void BM_SpscPushPop_ItchMsg(benchmark::State& state) {
    SpscQueue<ItchMsg, 65536> q;
    ItchMsg m{};
    m.type = MessageType::AddOrder;
    for (auto _ : state) {
        q.try_push(m);
        ItchMsg v;
        benchmark::DoNotOptimize(q.try_pop(v));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscPushPop_ItchMsg)->MinTime(2.0)->Unit(benchmark::kNanosecond);

// Cross-thread throughput (producer + consumer on separate threads)
static void BM_SpscThroughput(benchmark::State& state) {
    SpscQueue<uint16_t, 65536> q;
    std::atomic<bool> running{true};
    uint64_t consumed = 0;

    std::thread consumer([&] {
        uint16_t v;
        while (running.load(std::memory_order_relaxed)) {
            if (q.try_pop(v)) ++consumed;
        }
        // drain
        while (q.try_pop(v)) ++consumed;
    });

    for (auto _ : state) {
        while (!q.try_push(1)) {}
    }
    running.store(false);
    consumer.join();

    state.SetItemsProcessed(state.iterations());
    state.counters["consumed"] = static_cast<double>(consumed);
}
BENCHMARK(BM_SpscThroughput)->MinTime(3.0)->Unit(benchmark::kNanosecond)->UseRealTime();
