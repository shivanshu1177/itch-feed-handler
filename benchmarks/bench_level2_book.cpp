#include <benchmark/benchmark.h>
#include "orderbook/level2_book.hpp"
#include "protocol/itch_messages.hpp"
#include <cstdlib>
#include <cstring>

using namespace itch;

// Shared pool/map/book for all benchmarks
static constexpr uint32_t POOL_CAP = 1 << 21; // 2M orders
static constexpr uint32_t MAP_CAP  = 1 << 22; // 4M entries

struct BenchFixture {
    void*               pool_mem;
    void*               map_mem;
    OrderPool*          pool;
    OrderMap*           map;
    Level2Book*         book;

    BenchFixture() {
        pool_mem = std::aligned_alloc(64, POOL_CAP * (sizeof(Order) + sizeof(uint32_t)));
        map_mem  = std::aligned_alloc(16, MAP_CAP  * sizeof(MapEntry));
        std::memset(map_mem, 0, MAP_CAP * sizeof(MapEntry));
        pool = new OrderPool(POOL_CAP, pool_mem);
        map  = new OrderMap(static_cast<MapEntry*>(map_mem), MAP_CAP);
        book = new Level2Book(1, *pool, *map);
    }
    ~BenchFixture() {
        delete book;
        delete map;
        delete pool;
        std::free(pool_mem);
        std::free(map_mem);
    }

    ItchMsg make_add(OrderId ref, Side side, Price price, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::AddOrder;
        m.locate = 1;
        m.add_order.ref = ref; m.add_order.side = side;
        m.add_order.price = price; m.add_order.shares = qty;
        return m;
    }
    ItchMsg make_delete(OrderId ref) {
        ItchMsg m{};
        m.type = MessageType::OrderDelete;
        m.locate = 1;
        m.order_delete.ref = ref;
        return m;
    }
    ItchMsg make_cancel(OrderId ref, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::OrderCancel;
        m.locate = 1;
        m.order_cancel.ref = ref;
        m.order_cancel.cancelled_shares = qty;
        return m;
    }
    ItchMsg make_replace(OrderId orig, OrderId newref, Price price, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::OrderReplace;
        m.locate = 1;
        m.order_replace.orig_ref = orig;
        m.order_replace.new_ref = newref;
        m.order_replace.new_price = price;
        m.order_replace.new_shares = qty;
        return m;
    }
};

// BM_AddOrder: steady-state add (many price levels already exist)
static void BM_AddOrder(benchmark::State& state) {
    BenchFixture f;
    // Pre-populate 64 bid levels to simulate a live book
    for (int i = 0; i < 64; ++i)
        f.book->on_add_order(f.make_add(i + 1, Side::Buy, (100 - i) * 10000, 100));

    OrderId ref = 10000;
    for (auto _ : state) {
        f.book->on_add_order(f.make_add(ref, Side::Buy, 50 * 10000, 100));
        f.book->on_order_delete(f.make_delete(ref));
        ++ref;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddOrder)->MinTime(2.0)->Unit(benchmark::kNanosecond);

// BM_Delete: delete from middle of book
static void BM_Delete(benchmark::State& state) {
    BenchFixture f;
    OrderId base = 1;
    // Pre-fill 1000 orders
    for (int i = 0; i < 1000; ++i)
        f.book->on_add_order(f.make_add(base + i, Side::Buy, (100 - (i % 64)) * 10000, 100));

    OrderId del_ref = base;
    for (auto _ : state) {
        f.book->on_order_delete(f.make_delete(del_ref++));
        // Re-add to keep pool from exhausting
        f.book->on_add_order(f.make_add(del_ref + 1000, Side::Buy, 50 * 10000, 50));
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Delete)->MinTime(2.0)->Unit(benchmark::kNanosecond);

// BM_ReplaceSamePrice
static void BM_ReplaceSamePrice(benchmark::State& state) {
    BenchFixture f;
    f.book->on_add_order(f.make_add(1, Side::Buy, 100 * 10000, 100));

    OrderId orig = 1, newref = 100000;
    for (auto _ : state) {
        f.book->on_order_replace(f.make_replace(orig, newref, 100 * 10000, 200));
        std::swap(orig, newref);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReplaceSamePrice)->MinTime(2.0)->Unit(benchmark::kNanosecond);

// BM_ReplaceDifferentPrice
static void BM_ReplaceDifferentPrice(benchmark::State& state) {
    BenchFixture f;
    f.book->on_add_order(f.make_add(1, Side::Buy, 100 * 10000, 100));

    OrderId orig = 1, newref = 100000;
    Price   px   = 99 * 10000;
    for (auto _ : state) {
        f.book->on_order_replace(f.make_replace(orig, newref, px, 200));
        std::swap(orig, newref);
        px = (px == 99 * 10000) ? 100 * 10000 : 99 * 10000;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ReplaceDifferentPrice)->MinTime(2.0)->Unit(benchmark::kNanosecond);
