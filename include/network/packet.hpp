#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include <type_traits>

namespace itch {

struct ITCH_PACKED RawPacket {
    alignas(64) uint8_t data[MAX_UDP_PAYLOAD];
    uint16_t length{0};
    Timestamp recv_timestamp_ns{0};
};
static_assert(std::is_trivially_copyable_v<RawPacket>);

} // namespace itch
