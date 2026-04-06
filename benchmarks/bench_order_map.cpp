#include <benchmark/benchmark.h>
#include "orderbook/order_map.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace itch;

static constexpr uint32_t MAP_CAP = 1 << 22; // 4M entries

class MapBench {
  public:
    MapBench() {
        mem_ = static_cast<MapEntry*>(
            std::aligned_alloc(16, MAP_CAP * sizeof(MapEntry)));
        std::memset(mem_, 0, MAP_CAP * sizeof(MapEntry));
        map_ = new OrderMap(mem_, MAP_CAP);
    }
    ~MapBench() { delete map_; std::free(mem_); }

    OrderMap& map() { return *map_; }

  private:
    MapEntry* mem_;
    OrderMap* map_;
};

// Pre-fill 2M entries (50% load), measure find latency
static void BM_OrderMapFind(benchmark::State& state) {
    MapBench b;
    constexpr uint32_t N = 2000000;
    std::vector<OrderId> keys(N);
    for (uint32_t i = 0; i < N; ++i) {
        keys[i] = (static_cast<uint64_t>(i) + 1) * 7919; // spread keys
        b.map().insert(keys[i], i);
    }

    uint64_t idx = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.map().find(keys[idx % N]));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("50% load factor");
}
BENCHMARK(BM_OrderMapFind)->MinTime(2.0)->Unit(benchmark::kNanosecond);

static void BM_OrderMapInsert(benchmark::State& state) {
    MapBench b;
    uint64_t key = 1;
    uint32_t val = 0;

    for (auto _ : state) {
        b.map().insert(key, val);
        ++key; ++val;
        if (val >= MAP_CAP / 2) {
            // Reset to avoid exhausting capacity
            val = 0;
            key = 1;
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OrderMapInsert)->MinTime(2.0)->Unit(benchmark::kNanosecond);
