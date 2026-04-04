#include <benchmark/benchmark.h>
#include "protocol/itch_parser.hpp"
#include "protocol/moldudp64_parser.hpp"
#include "protocol/itch_messages.hpp"
#include <cstring>
#include <vector>

using namespace itch;

// Build a realistic MoldUDP64 packet with N AddOrder messages
static std::vector<uint8_t> build_packet(int n_msgs) {
    // MoldUDP64 header (20) + N * (2 + 36) bytes
    std::vector<uint8_t> pkt(20 + n_msgs * 38, 0);
    uint8_t* p = pkt.data();

    std::memcpy(p, "TESTSESS01", 10); p += 10;
    uint64_t seq = 1;
    for (int i = 7; i >= 0; --i) { p[i] = seq & 0xFF; seq >>= 8; }
    p += 8;
    p[0] = static_cast<uint8_t>(n_msgs >> 8);
    p[1] = static_cast<uint8_t>(n_msgs & 0xFF);
    p += 2;

    for (int m = 0; m < n_msgs; ++m) {
        // MessageBlock length
        p[0] = 0; p[1] = 36; p += 2;

        WireAddOrder w{};
        w.type     = 'A';
        w.locate[1] = 1;
        uint64_t ref = static_cast<uint64_t>(m + 1);
        for (int i = 7; i >= 0; --i) { w.order_ref[i] = ref & 0xFF; ref >>= 8; }
        w.buy_sell   = 'B';
        w.shares[3]  = 100;
        std::memcpy(w.stock, "AAPL    ", 8);
        uint32_t px = 1500000;
        w.price[0]=(px>>24)&0xFF; w.price[1]=(px>>16)&0xFF;
        w.price[2]=(px>>8)&0xFF;  w.price[3]=px&0xFF;
        std::memcpy(p, &w, 36); p += 36;
    }
    return pkt;
}

static void BM_ParseSingleAddOrder(benchmark::State& state) {
    auto pkt = build_packet(1);
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ItchMsg buf[128];

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            ItchParser::parse(pkt.data(), static_cast<uint16_t>(pkt.size()),
                              0, mold, buf, 128));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("single AddOrder");
}
BENCHMARK(BM_ParseSingleAddOrder)->MinTime(2.0)->Unit(benchmark::kNanosecond);

static void BM_ParseMultiMsg(benchmark::State& state) {
    int n = static_cast<int>(state.range(0));
    auto pkt = build_packet(n);
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ItchMsg buf[128];

    for (auto _ : state) {
        benchmark::DoNotOptimize(
            ItchParser::parse(pkt.data(), static_cast<uint16_t>(pkt.size()),
                              0, mold, buf, 128));
    }
    state.SetItemsProcessed(state.iterations() * n);
    state.SetLabel("msgs per packet");
}
BENCHMARK(BM_ParseMultiMsg)->Arg(1)->Arg(4)->Arg(16)->Arg(32)
    ->MinTime(2.0)->Unit(benchmark::kNanosecond);
