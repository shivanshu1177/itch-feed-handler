#pragma once
#include <cstdint>

namespace itch {

using OrderId = uint64_t;    // ITCH order reference number
using Price = int64_t;       // Fixed-point price × 10000 (signed for spread calc)
using Qty = uint32_t;        // Share quantity
using LocateCode = uint16_t; // Stock locate code (0–65535)
using ShardId = uint8_t;     // Shard index (0–NUM_SHARDS-1)
using Timestamp = uint64_t;  // Nanoseconds (from midnight or monotonic)

enum class Side : char { Buy = 'B', Sell = 'S' };

enum class MessageType : char {
    SystemEvent = 'S',
    StockDirectory = 'R',
    StockTradingAction = 'H',
    RegSHO = 'Y',
    MarketParticipant = 'L',
    MWCBDecline = 'V',
    MWCBStatus = 'W',
    IPOQuotingPeriod = 'K',
    LULDAuctionCollar = 'J',
    OperationalHalt = 'h',
    AddOrder = 'A',
    AddOrderMPID = 'F',
    OrderExecuted = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
    Trade = 'P',
    CrossTrade = 'Q',
    BrokenTrade = 'B',
    NOII = 'I',
    RPIIndicator = 'N',
    Unknown = '?',
    // Internal sentinel — never appears on the wire.
    // Broadcast by SessionManager::reset_session() to instruct every
    // ShardWorker to clear its books, pool, and map.
    InternalSessionReset = '\x01',
};

} // namespace itch
