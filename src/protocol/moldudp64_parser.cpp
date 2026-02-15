#include "moldudp64_parser.hpp"
#include <cstring>

namespace itch {

MoldUDP64Parser::ParseResult MoldUDP64Parser::parse(const uint8_t* data, std::size_t length) {
    ParseResult result;
    if (length < sizeof(MoldUDP64Header)) {
        return result;
    }
    std::memcpy(&result.header, data, sizeof(MoldUDP64Header));
    if (result.header.is_heartbeat()) {
        ITCH_LOG_DEBUG("MoldUDP64 heartbeat seq=%lu", result.header.get_sequence());
        result.valid = true;
        return result;
    }
    result.messages_begin = data + sizeof(MoldUDP64Header);
    result.messages_length = length - sizeof(MoldUDP64Header);
    result.valid = true;
    return result;
}

} // namespace itch
