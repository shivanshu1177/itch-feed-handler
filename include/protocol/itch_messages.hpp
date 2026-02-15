#pragma once
#include "../common/compiler.hpp"
#include "../common/types.hpp"
#include <cstdint>
#include <type_traits>

// ============================================================
// ITCH 5.0 Wire Format Definitions
//
// All multi-byte integer fields are BIG-ENDIAN on the wire.
// Timestamps are 48-bit (6 bytes), nanoseconds since midnight.
// Prices are 32-bit fixed-point, implicit 4 decimal places
//   (divide by 10000 to get dollars).
// ============================================================

namespace itch {

// -----------------------------------------------------------------
// Byte-swap helpers (big-endian wire → host, called only in parser)
// -----------------------------------------------------------------
namespace wire {

ITCH_FORCE_INLINE uint16_t be16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
ITCH_FORCE_INLINE uint32_t be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
ITCH_FORCE_INLINE uint64_t be48(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 40) | (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) | (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) << 8) | p[5];
}
ITCH_FORCE_INLINE uint64_t be64(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56) | (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) | (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) | (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) | p[7];
}

} // namespace wire

// -----------------------------------------------------------------
// Common message header (every ITCH message starts with this)
// -----------------------------------------------------------------
// Offset  Size  Field
//    0      1   Message Type
//    1      2   Stock Locate (BE)
//    3      2   Tracking Number (BE)
//    5      6   Timestamp — ns from midnight (48-bit BE)
//   11      …   Message-specific fields
// -----------------------------------------------------------------

// Offsets into raw message data for common header fields
static constexpr int kOffType      = 0;
static constexpr int kOffLocate    = 1;
static constexpr int kOffTracking  = 3;
static constexpr int kOffTimestamp = 5; // 6 bytes
static constexpr int kOffPayload   = 11; // first message-specific byte

// -----------------------------------------------------------------
// Wire format structs — packed, no padding, big-endian fields
// Each struct byte-layout matches the ITCH 5.0 spec exactly.
// Accessor functions handle endian conversion.
// -----------------------------------------------------------------

struct ITCH_PACKED WireSystemEvent {       // 'S'  12 bytes
    uint8_t  type;           // 0
    uint8_t  locate[2];      // 1
    uint8_t  tracking[2];    // 3
    uint8_t  timestamp[6];   // 5
    uint8_t  event_code;     // 11  O=Start,S=Start-hours,Q=End-hours,M=End,E=Emergency,C=Resumed
};
static_assert(sizeof(WireSystemEvent) == 12);

struct ITCH_PACKED WireStockDirectory {   // 'R'  39 bytes
    uint8_t  type;           // 0
    uint8_t  locate[2];      // 1
    uint8_t  tracking[2];    // 3
    uint8_t  timestamp[6];   // 5
    char     stock[8];       // 11  ASCII right-padded with spaces
    uint8_t  market_category; // 19 T=NasdaqGS Q=NasdaqGM S=NasdaqCM N=NYSE A=NYSE-MKT P=NYSE-Arca
    uint8_t  financial_status; // 20 D=Deficient E=Delinquent Q=Bankrupt S=Suspended G=IG H=HaltD L=HaltLP O=OP X=OP-Halt space=N/A
    uint8_t  round_lot_size[4]; // 21
    uint8_t  round_lots_only;   // 25
    uint8_t  issue_classification; // 26
    uint8_t  issue_sub_type[2];    // 27
    uint8_t  authenticity;         // 29 P=Live N=Test
    uint8_t  short_sale_threshold; // 30 Y=Threshold N=Not space=N/A
    uint8_t  ipo_flag;             // 31 Y=FirstDay N=Not space=N/A
    uint8_t  luld_tier;            // 32 1=Tier1 2=Tier2 space=N/A
    uint8_t  etp_flag;             // 33 Y=ETP N=Not space=N/A
    uint8_t  etp_leverage[4];      // 34
    uint8_t  inverse_indicator;    // 38 Y=Inverse N=Not
};
static_assert(sizeof(WireStockDirectory) == 39);

struct ITCH_PACKED WireStockTradingAction { // 'H'  25 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  trading_state; // H=Halted P=Paused Q=Quotation T=Trading
    uint8_t  reserved;
    char     reason[4];
};
static_assert(sizeof(WireStockTradingAction) == 25);

struct ITCH_PACKED WireRegSHO {            // 'Y'  20 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  reg_sho_action; // 0=No restriction 1=Short sale price test E=End
};
static_assert(sizeof(WireRegSHO) == 20);

struct ITCH_PACKED WireMarketParticipant { // 'L'  26 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     mpid[4];
    char     stock[8];
    uint8_t  primary_mm;    // Y=Primary N=Non-primary
    uint8_t  mm_mode;       // N=Normal P=Passive S=Syndicate R=Pre-syndicate L=Penalty
    uint8_t  participant_state; // A=Active E=Excused W=Withdrawn S=Suspended D=Deleted
};
static_assert(sizeof(WireMarketParticipant) == 26);

struct ITCH_PACKED WireMWCBDecline {       // 'V'  35 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  level1[8];    // 7% decline price (fixed-point ×10000)
    uint8_t  level2[8];    // 13% decline
    uint8_t  level3[8];    // 20% decline
};
static_assert(sizeof(WireMWCBDecline) == 35);

struct ITCH_PACKED WireMWCBStatus {        // 'W'  12 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  breached_level; // 1, 2, or 3
};
static_assert(sizeof(WireMWCBStatus) == 12);

struct ITCH_PACKED WireIPOQuoting {        // 'K'  28 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  ipo_quotation_release_time[4]; // seconds past midnight
    uint8_t  ipo_quotation_release_qualifier; // A=Anticipated Q=IPO release
    uint8_t  ipo_price[4];
};
static_assert(sizeof(WireIPOQuoting) == 28);

struct ITCH_PACKED WireLULDAuctionCollar { // 'J'  35 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  auction_collar_ref_price[4];
    uint8_t  upper_auction_collar_price[4];
    uint8_t  lower_auction_collar_price[4];
    uint8_t  auction_collar_extension[4];
};
static_assert(sizeof(WireLULDAuctionCollar) == 35);

struct ITCH_PACKED WireOperationalHalt {   // 'h'  21 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  market_code;          // Q=NASDAQ B=BX PSX
    uint8_t  operational_halt_action; // H=Halted T=Trading
};
static_assert(sizeof(WireOperationalHalt) == 21);

struct ITCH_PACKED WireAddOrder {          // 'A'  36 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];  // order reference number
    uint8_t  buy_sell;      // 'B' or 'S'
    uint8_t  shares[4];
    char     stock[8];
    uint8_t  price[4];      // fixed-point ×10000
};
static_assert(sizeof(WireAddOrder) == 36);

struct ITCH_PACKED WireAddOrderMPID {      // 'F'  40 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
    uint8_t  buy_sell;
    uint8_t  shares[4];
    char     stock[8];
    uint8_t  price[4];
    char     attribution[4]; // MPID
};
static_assert(sizeof(WireAddOrderMPID) == 40);

struct ITCH_PACKED WireOrderExecuted {     // 'E'  31 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
    uint8_t  executed_shares[4];
    uint8_t  match_number[8];
};
static_assert(sizeof(WireOrderExecuted) == 31);

struct ITCH_PACKED WireOrderExecutedPrice { // 'C'  36 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
    uint8_t  executed_shares[4];
    uint8_t  match_number[8];
    uint8_t  printable;       // 'Y' or 'N'
    uint8_t  execution_price[4];
};
static_assert(sizeof(WireOrderExecutedPrice) == 36);

struct ITCH_PACKED WireOrderCancel {       // 'X'  23 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
    uint8_t  cancelled_shares[4];
};
static_assert(sizeof(WireOrderCancel) == 23);

struct ITCH_PACKED WireOrderDelete {       // 'D'  19 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
};
static_assert(sizeof(WireOrderDelete) == 19);

struct ITCH_PACKED WireOrderReplace {      // 'U'  35 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  orig_order_ref[8];
    uint8_t  new_order_ref[8];
    uint8_t  new_shares[4];
    uint8_t  new_price[4];
};
static_assert(sizeof(WireOrderReplace) == 35);

struct ITCH_PACKED WireTrade {             // 'P'  44 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  order_ref[8];
    uint8_t  buy_sell;
    uint8_t  shares[4];
    char     stock[8];
    uint8_t  price[4];
    uint8_t  match_number[8];
};
static_assert(sizeof(WireTrade) == 44);

struct ITCH_PACKED WireCrossTrade {        // 'Q'  40 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  shares[8];     // cross qty is 8 bytes
    char     stock[8];
    uint8_t  cross_price[4];
    uint8_t  match_number[8];
    uint8_t  cross_type;    // O=Open I=Intraday C=Closing H=IPO/Halt
};
static_assert(sizeof(WireCrossTrade) == 40);

struct ITCH_PACKED WireBrokenTrade {       // 'B'  19 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  match_number[8];
};
static_assert(sizeof(WireBrokenTrade) == 19);

struct ITCH_PACKED WireNOII {              // 'I'  50 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    uint8_t  paired_shares[8];
    uint8_t  imbalance_shares[8];
    uint8_t  imbalance_direction; // B=Buy S=Sell N=No imbalance O=Insufficient
    char     stock[8];
    uint8_t  far_price[4];
    uint8_t  near_price[4];
    uint8_t  current_ref_price[4];
    uint8_t  cross_type;
    uint8_t  price_variation_indicator;
};
static_assert(sizeof(WireNOII) == 50);

struct ITCH_PACKED WireRPIIndicator {      // 'N'  20 bytes
    uint8_t  type;
    uint8_t  locate[2];
    uint8_t  tracking[2];
    uint8_t  timestamp[6];
    char     stock[8];
    uint8_t  interest_flag; // B=Buy S=Sell A=Both N=None
};
static_assert(sizeof(WireRPIIndicator) == 20);

// -----------------------------------------------------------------
// Normalized host-byte-order message (after parsing from wire)
// Contains only the fields needed for order book processing.
// -----------------------------------------------------------------
struct ItchMsg {
    MessageType type     = MessageType::Unknown;
    LocateCode  locate   = 0;
    Timestamp   timestamp = 0;       // ns from midnight (host byte order)
    Timestamp   recv_timestamp = 0;  // ns monotonic, set by receiver

    union {
        // AddOrder / AddOrderMPID
        struct {
            OrderId ref;
            Side    side;
            Price   price;  // host order, fixed-point ×10000
            Qty     shares;
            char    stock[8];
            char    attribution[4]; // populated only for AddOrderMPID
        } add_order;

        // OrderExecuted
        struct {
            OrderId  ref;
            Qty      executed_shares;
            uint64_t match_number;
        } order_executed;

        // OrderExecutedPrice
        struct {
            OrderId  ref;
            Qty      executed_shares;
            uint64_t match_number;
            Price    price;
            bool     printable;
        } order_executed_price;

        // OrderCancel
        struct {
            OrderId ref;
            Qty     cancelled_shares;
        } order_cancel;

        // OrderDelete
        struct {
            OrderId ref;
        } order_delete;

        // OrderReplace
        struct {
            OrderId orig_ref;
            OrderId new_ref;
            Qty     new_shares;
            Price   new_price;
        } order_replace;

        // StockDirectory
        struct {
            char     stock[8];
            uint8_t  market_category;
            uint32_t round_lot_size;
            uint8_t  financial_status;
            uint8_t  authenticity; // 'P'=live 'N'=test
        } stock_directory;

        // StockTradingAction
        struct {
            char    stock[8];
            uint8_t trading_state; // 'H'=halted 'P'=paused 'Q'=quotation 'T'=trading
            char    reason[4];
        } trading_action;

        // Trade (non-cross)
        struct {
            OrderId  ref;
            Side     side;
            Qty      shares;
            Price    price;
            char     stock[8];
            uint64_t match_number;
        } trade;

        // CrossTrade
        struct {
            uint64_t shares;
            Price    cross_price;
            char     stock[8];
            uint64_t match_number;
            uint8_t  cross_type;
        } cross_trade;

        // BrokenTrade
        struct {
            uint64_t match_number;
        } broken_trade;

        // NOII
        struct {
            uint64_t paired_shares;
            uint64_t imbalance_shares;
            uint8_t  imbalance_direction;
            char     stock[8];
            Price    far_price;
            Price    near_price;
            Price    current_ref_price;
            uint8_t  cross_type;
            uint8_t  price_variation_indicator;
        } noii;

        // SystemEvent
        struct {
            uint8_t event_code;
        } system_event;

        // RegSHO
        struct {
            char    stock[8];
            uint8_t action;
        } reg_sho;

        // RPIIndicator
        struct {
            char    stock[8];
            uint8_t interest_flag;
        } rpi;
    };

    ItchMsg() = default;
};
static_assert(std::is_trivially_copyable_v<ItchMsg>);

} // namespace itch
