#include <gtest/gtest.h>
#include "protocol/itch_parser.hpp"
#include "protocol/moldudp64.hpp"
#include "protocol/itch_messages.hpp"
#include <cstring>
#include <arpa/inet.h>

using namespace itch;

// Helper: build a minimal MoldUDP64 packet containing one ITCH message
static std::vector<uint8_t> make_packet(const uint8_t* msg_data, uint16_t msg_len) {
    // MoldUDP64 header: session[10] + seq[8 BE] + count[2 BE] = 20 bytes
    // MessageBlock header: len[2 BE] = 2 bytes
    // Total: 22 + msg_len
    std::vector<uint8_t> pkt(22 + msg_len, 0);
    uint8_t* p = pkt.data();

    // session "TESTSESS01"
    std::memcpy(p, "TESTSESS01", 10); p += 10;
    // seq = 1 big-endian
    uint64_t seq = 1;
    for (int i = 7; i >= 0; --i) { p[i] = seq & 0xFF; seq >>= 8; }
    p += 8;
    // count = 1
    p[0] = 0; p[1] = 1; p += 2;

    // MessageBlock header: length
    p[0] = static_cast<uint8_t>(msg_len >> 8);
    p[1] = static_cast<uint8_t>(msg_len & 0xFF);
    p += 2;

    std::memcpy(p, msg_data, msg_len);
    return pkt;
}

TEST(ItchParser, ParseAddOrder) {
    // Build WireAddOrder (36 bytes)
    WireAddOrder w{};
    w.type      = 'A';
    // locate[2] BE = 1
    w.locate[0] = 0; w.locate[1] = 1;
    // tracking[2] = 0
    // timestamp[6] = 0
    // order_ref[8] BE = 12345
    uint64_t ref = 12345;
    for (int i = 7; i >= 0; --i) { w.order_ref[i] = ref & 0xFF; ref >>= 8; }
    w.buy_sell   = 'B';
    // shares[4] BE = 100
    w.shares[0]=0; w.shares[1]=0; w.shares[2]=0; w.shares[3]=100;
    std::memcpy(w.stock, "AAPL    ", 8);
    // price[4] BE = 150*10000 = 1500000
    uint32_t px = 1500000;
    w.price[0] = (px>>24)&0xFF; w.price[1]=(px>>16)&0xFF;
    w.price[2] = (px>>8)&0xFF;  w.price[3]=px&0xFF;

    auto pkt = make_packet(reinterpret_cast<const uint8_t*>(&w), sizeof(w));
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ASSERT_TRUE(mold.valid);

    ItchMsg buf[10];
    int count = ItchParser::parse(pkt.data(), static_cast<uint16_t>(pkt.size()),
                                   0, mold, buf, 10);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(buf[0].type, MessageType::AddOrder);
    EXPECT_EQ(buf[0].locate, 1u);
    EXPECT_EQ(buf[0].add_order.ref, 12345u);
    EXPECT_EQ(buf[0].add_order.side, Side::Buy);
    EXPECT_EQ(buf[0].add_order.shares, 100u);
    EXPECT_EQ(buf[0].add_order.price, 1500000);
}

TEST(ItchParser, ParseOrderDelete) {
    WireOrderDelete w{};
    w.type = 'D';
    w.locate[1] = 2;
    uint64_t ref = 9999;
    for (int i = 7; i >= 0; --i) { w.order_ref[i] = ref & 0xFF; ref >>= 8; }

    auto pkt = make_packet(reinterpret_cast<const uint8_t*>(&w), sizeof(w));
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ItchMsg buf[10];
    int count = ItchParser::parse(pkt.data(), static_cast<uint16_t>(pkt.size()),
                                   0, mold, buf, 10);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(buf[0].type, MessageType::OrderDelete);
    EXPECT_EQ(buf[0].order_delete.ref, 9999u);
}

TEST(ItchParser, Heartbeat) {
    // MoldUDP64 heartbeat: message_count = 0xFFFF
    std::vector<uint8_t> pkt(20, 0);
    pkt[18] = 0xFF; pkt[19] = 0xFF; // count = 0xFFFF = heartbeat
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ASSERT_TRUE(mold.valid);
    ASSERT_TRUE(mold.header.is_heartbeat());

    ItchMsg buf[10];
    int count = ItchParser::parse(pkt.data(), 20, 0, mold, buf, 10);
    EXPECT_EQ(count, 0);
}

TEST(ItchParser, TruncatedMessage) {
    // Provide only half of a WireAddOrder — parser should not overread
    uint8_t half[18] = {'A', 0, 1, 0, 0, 0,0,0,0,0,0,0, 0,0,0,0,0,0};
    auto pkt = make_packet(half, sizeof(half));
    auto mold = MoldUDP64Parser::parse(pkt.data(), pkt.size());
    ItchMsg buf[10];
    // Should parse 1 message but with type=AddOrder and fail size check → count=0
    int count = ItchParser::parse(pkt.data(), static_cast<uint16_t>(pkt.size()),
                                   0, mold, buf, 10);
    EXPECT_EQ(count, 0); // truncated, no valid messages
}
