#include "itch_parser.hpp"
#include "../common/logging.hpp"
#include <cstring>

namespace itch {

int ItchParser::parse(const uint8_t* /*pkt_data*/, uint16_t /*pkt_len*/, Timestamp recv_ts,
                      const MoldUDP64Parser::ParseResult& mold_result,
                      ItchMsg* out_buf, int max_msgs) noexcept {
    if (!mold_result.valid || max_msgs <= 0)
        return 0;

    const uint8_t* ptr = mold_result.messages_begin;
    std::size_t remaining = mold_result.messages_length;
    int count = 0;

    while (remaining > 0 && count < max_msgs) {
        if (remaining < sizeof(MoldUDP64MessageBlock))
            break;

        const auto* block = reinterpret_cast<const MoldUDP64MessageBlock*>(ptr);
        uint16_t msg_len = block->get_length();

        if (msg_len == 0)
            break;
        if (msg_len > remaining - sizeof(MoldUDP64MessageBlock))
            break; // truncated packet

        ItchMsg& msg = out_buf[count];
        msg = ItchMsg{};
        msg.recv_timestamp = recv_ts;

        if (parse_message(msg, block->payload(), msg_len)) {
            ++count;
        }

        ptr += sizeof(MoldUDP64MessageBlock) + msg_len;
        remaining -= sizeof(MoldUDP64MessageBlock) + msg_len;
    }

    return count;
}

bool ItchParser::parse_message(ItchMsg& msg, const uint8_t* data, uint16_t len) noexcept {
    if (!data || len < 1)
        return false;

    const char type_char = static_cast<char>(data[kOffType]);
    msg.type   = static_cast<MessageType>(type_char);
    msg.locate = static_cast<LocateCode>(wire::be16(data + kOffLocate));

    // All messages with the standard header have 6-byte timestamp at offset 5
    if (len >= kOffPayload) {
        msg.timestamp = wire::be48(data + kOffTimestamp);
    }

    switch (msg.type) {
    // ------------------------------------------------------------------
    // System Event  'S'  12 bytes
    // ------------------------------------------------------------------
    case MessageType::SystemEvent: {
        if (len < sizeof(WireSystemEvent))
            return false;
        const auto* w = reinterpret_cast<const WireSystemEvent*>(data);
        msg.system_event.event_code = w->event_code;
        return true;
    }

    // ------------------------------------------------------------------
    // Stock Directory  'R'  39 bytes
    // ------------------------------------------------------------------
    case MessageType::StockDirectory: {
        if (len < sizeof(WireStockDirectory))
            return false;
        const auto* w = reinterpret_cast<const WireStockDirectory*>(data);
        std::memcpy(msg.stock_directory.stock, w->stock, 8);
        msg.stock_directory.market_category = w->market_category;
        msg.stock_directory.financial_status = w->financial_status;
        msg.stock_directory.round_lot_size = wire::be32(w->round_lot_size);
        msg.stock_directory.authenticity = w->authenticity;
        return true;
    }

    // ------------------------------------------------------------------
    // Stock Trading Action  'H'  25 bytes
    // ------------------------------------------------------------------
    case MessageType::StockTradingAction: {
        if (len < sizeof(WireStockTradingAction))
            return false;
        const auto* w = reinterpret_cast<const WireStockTradingAction*>(data);
        std::memcpy(msg.trading_action.stock, w->stock, 8);
        msg.trading_action.trading_state = w->trading_state;
        std::memcpy(msg.trading_action.reason, w->reason, 4);
        return true;
    }

    // ------------------------------------------------------------------
    // Reg SHO Short Sale Price Test Restriction  'Y'  20 bytes
    // ------------------------------------------------------------------
    case MessageType::RegSHO: {
        if (len < sizeof(WireRegSHO))
            return false;
        const auto* w = reinterpret_cast<const WireRegSHO*>(data);
        std::memcpy(msg.reg_sho.stock, w->stock, 8);
        msg.reg_sho.action = w->reg_sho_action;
        return true;
    }

    // ------------------------------------------------------------------
    // Market Participant Position  'L'  26 bytes
    // (informational only — no order book impact)
    // ------------------------------------------------------------------
    case MessageType::MarketParticipant: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // MWCB Decline Level  'V'  35 bytes
    // ------------------------------------------------------------------
    case MessageType::MWCBDecline: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // MWCB Status  'W'  12 bytes
    // ------------------------------------------------------------------
    case MessageType::MWCBStatus: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // IPO Quoting Period Update  'K'  28 bytes
    // ------------------------------------------------------------------
    case MessageType::IPOQuotingPeriod: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // LULD Auction Collar  'J'  35 bytes
    // ------------------------------------------------------------------
    case MessageType::LULDAuctionCollar: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // Operational Halt  'h'  21 bytes
    // ------------------------------------------------------------------
    case MessageType::OperationalHalt: {
        return true; // parse but discard
    }

    // ------------------------------------------------------------------
    // Add Order (No MPID)  'A'  36 bytes
    // ------------------------------------------------------------------
    case MessageType::AddOrder: {
        if (len < sizeof(WireAddOrder))
            return false;
        const auto* w = reinterpret_cast<const WireAddOrder*>(data);
        msg.add_order.ref    = wire::be64(w->order_ref);
        msg.add_order.side   = static_cast<Side>(w->buy_sell);
        msg.add_order.shares = wire::be32(w->shares);
        msg.add_order.price  = static_cast<Price>(wire::be32(w->price));
        std::memcpy(msg.add_order.stock, w->stock, 8);
        std::memset(msg.add_order.attribution, 0, 4);
        return true;
    }

    // ------------------------------------------------------------------
    // Add Order with MPID  'F'  40 bytes
    // ------------------------------------------------------------------
    case MessageType::AddOrderMPID: {
        if (len < sizeof(WireAddOrderMPID))
            return false;
        const auto* w = reinterpret_cast<const WireAddOrderMPID*>(data);
        msg.add_order.ref    = wire::be64(w->order_ref);
        msg.add_order.side   = static_cast<Side>(w->buy_sell);
        msg.add_order.shares = wire::be32(w->shares);
        msg.add_order.price  = static_cast<Price>(wire::be32(w->price));
        std::memcpy(msg.add_order.stock, w->stock, 8);
        std::memcpy(msg.add_order.attribution, w->attribution, 4);
        return true;
    }

    // ------------------------------------------------------------------
    // Order Executed  'E'  31 bytes
    // ------------------------------------------------------------------
    case MessageType::OrderExecuted: {
        if (len < sizeof(WireOrderExecuted))
            return false;
        const auto* w = reinterpret_cast<const WireOrderExecuted*>(data);
        msg.order_executed.ref              = wire::be64(w->order_ref);
        msg.order_executed.executed_shares  = wire::be32(w->executed_shares);
        msg.order_executed.match_number     = wire::be64(w->match_number);
        return true;
    }

    // ------------------------------------------------------------------
    // Order Executed with Price  'C'  36 bytes
    // ------------------------------------------------------------------
    case MessageType::OrderExecutedPrice: {
        if (len < sizeof(WireOrderExecutedPrice))
            return false;
        const auto* w = reinterpret_cast<const WireOrderExecutedPrice*>(data);
        msg.order_executed_price.ref             = wire::be64(w->order_ref);
        msg.order_executed_price.executed_shares = wire::be32(w->executed_shares);
        msg.order_executed_price.match_number    = wire::be64(w->match_number);
        msg.order_executed_price.printable       = (w->printable == 'Y');
        msg.order_executed_price.price           = static_cast<Price>(wire::be32(w->execution_price));
        return true;
    }

    // ------------------------------------------------------------------
    // Order Cancel  'X'  23 bytes
    // ------------------------------------------------------------------
    case MessageType::OrderCancel: {
        if (len < sizeof(WireOrderCancel))
            return false;
        const auto* w = reinterpret_cast<const WireOrderCancel*>(data);
        msg.order_cancel.ref              = wire::be64(w->order_ref);
        msg.order_cancel.cancelled_shares = wire::be32(w->cancelled_shares);
        return true;
    }

    // ------------------------------------------------------------------
    // Order Delete  'D'  19 bytes
    // ------------------------------------------------------------------
    case MessageType::OrderDelete: {
        if (len < sizeof(WireOrderDelete))
            return false;
        const auto* w = reinterpret_cast<const WireOrderDelete*>(data);
        msg.order_delete.ref = wire::be64(w->order_ref);
        return true;
    }

    // ------------------------------------------------------------------
    // Order Replace  'U'  35 bytes
    // ------------------------------------------------------------------
    case MessageType::OrderReplace: {
        if (len < sizeof(WireOrderReplace))
            return false;
        const auto* w = reinterpret_cast<const WireOrderReplace*>(data);
        msg.order_replace.orig_ref   = wire::be64(w->orig_order_ref);
        msg.order_replace.new_ref    = wire::be64(w->new_order_ref);
        msg.order_replace.new_shares = wire::be32(w->new_shares);
        msg.order_replace.new_price  = static_cast<Price>(wire::be32(w->new_price));
        return true;
    }

    // ------------------------------------------------------------------
    // Trade Message (non-cross)  'P'  44 bytes
    // ------------------------------------------------------------------
    case MessageType::Trade: {
        if (len < sizeof(WireTrade))
            return false;
        const auto* w = reinterpret_cast<const WireTrade*>(data);
        msg.trade.ref          = wire::be64(w->order_ref);
        msg.trade.side         = static_cast<Side>(w->buy_sell);
        msg.trade.shares       = wire::be32(w->shares);
        msg.trade.price        = static_cast<Price>(wire::be32(w->price));
        std::memcpy(msg.trade.stock, w->stock, 8);
        msg.trade.match_number = wire::be64(w->match_number);
        return true;
    }

    // ------------------------------------------------------------------
    // Cross Trade  'Q'  40 bytes
    // ------------------------------------------------------------------
    case MessageType::CrossTrade: {
        if (len < sizeof(WireCrossTrade))
            return false;
        const auto* w = reinterpret_cast<const WireCrossTrade*>(data);
        msg.cross_trade.shares        = wire::be64(w->shares);
        msg.cross_trade.cross_price   = static_cast<Price>(wire::be32(w->cross_price));
        std::memcpy(msg.cross_trade.stock, w->stock, 8);
        msg.cross_trade.match_number  = wire::be64(w->match_number);
        msg.cross_trade.cross_type    = w->cross_type;
        return true;
    }

    // ------------------------------------------------------------------
    // Broken Trade / Order Execution  'B'  19 bytes
    // ------------------------------------------------------------------
    case MessageType::BrokenTrade: {
        if (len < sizeof(WireBrokenTrade))
            return false;
        const auto* w = reinterpret_cast<const WireBrokenTrade*>(data);
        msg.broken_trade.match_number = wire::be64(w->match_number);
        return true;
    }

    // ------------------------------------------------------------------
    // Net Order Imbalance Indicator  'I'  50 bytes
    // ------------------------------------------------------------------
    case MessageType::NOII: {
        if (len < sizeof(WireNOII))
            return false;
        const auto* w = reinterpret_cast<const WireNOII*>(data);
        msg.noii.paired_shares           = wire::be64(w->paired_shares);
        msg.noii.imbalance_shares        = wire::be64(w->imbalance_shares);
        msg.noii.imbalance_direction     = w->imbalance_direction;
        std::memcpy(msg.noii.stock, w->stock, 8);
        msg.noii.far_price               = static_cast<Price>(wire::be32(w->far_price));
        msg.noii.near_price              = static_cast<Price>(wire::be32(w->near_price));
        msg.noii.current_ref_price       = static_cast<Price>(wire::be32(w->current_ref_price));
        msg.noii.cross_type              = w->cross_type;
        msg.noii.price_variation_indicator = w->price_variation_indicator;
        return true;
    }

    // ------------------------------------------------------------------
    // Retail Price Improvement Indicator  'N'  20 bytes
    // ------------------------------------------------------------------
    case MessageType::RPIIndicator: {
        if (len < sizeof(WireRPIIndicator))
            return false;
        const auto* w = reinterpret_cast<const WireRPIIndicator*>(data);
        std::memcpy(msg.rpi.stock, w->stock, 8);
        msg.rpi.interest_flag = w->interest_flag;
        return true;
    }

    default:
        ITCH_LOG_DEBUG("Unknown ITCH message type '%c' (0x%02x), len=%u",
                       type_char, static_cast<uint8_t>(type_char), len);
        return false;
    }
}

} // namespace itch
