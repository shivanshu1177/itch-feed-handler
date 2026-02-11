#pragma once
#include "../common/compiler.hpp"
#include "../common/types.hpp"
#include "itch_messages.hpp"
#include "moldudp64_parser.hpp"
#include <cstdint>

namespace itch {

class ItchParser {
  public:
    // Maximum ITCH messages in a single MoldUDP64 packet (spec: 65535, practical: ~65)
    static constexpr int MAX_MSGS_PER_PACKET = 128;

    // Zero-allocation parse: writes into caller-provided buffer.
    // Returns the number of messages written (<= max_msgs).
    // recv_ts is stamped onto every produced ItchMsg.
    static int parse(const uint8_t* pkt_data, uint16_t pkt_len, Timestamp recv_ts,
                     const MoldUDP64Parser::ParseResult& mold_result,
                     ItchMsg* out_buf, int max_msgs) noexcept;

  private:
    // Parse a single ITCH message from raw bytes into msg.
    // Returns true on success, false if message type is unknown/unsupported.
    static bool parse_message(ItchMsg& msg, const uint8_t* data, uint16_t len) noexcept;
};

} // namespace itch
