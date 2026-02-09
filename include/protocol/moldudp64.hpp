#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include <cstdint>

namespace itch {

struct ITCH_PACKED MoldUDP64Header {
    char session[10];
    uint64_t sequence_number; // big-endian
    uint16_t message_count;   // big-endian, 0xFFFF = heartbeat

    ITCH_FORCE_INLINE uint64_t get_sequence() const { return __builtin_bswap64(sequence_number); }

    ITCH_FORCE_INLINE uint16_t get_message_count() const {
        return __builtin_bswap16(message_count);
    }

    ITCH_FORCE_INLINE bool is_heartbeat() const {
        return get_message_count() == MOLDUDP64_HEARTBEAT;
    }
};
static_assert(sizeof(MoldUDP64Header) == 20);

struct ITCH_PACKED MoldUDP64MessageBlock {
    uint16_t message_length; // big-endian

    ITCH_FORCE_INLINE uint16_t get_length() const { return __builtin_bswap16(message_length); }

    ITCH_FORCE_INLINE const uint8_t* payload() const {
        return reinterpret_cast<const uint8_t*>(this) + sizeof(MoldUDP64MessageBlock);
    }
};
static_assert(sizeof(MoldUDP64MessageBlock) == 2);

} // namespace itch
