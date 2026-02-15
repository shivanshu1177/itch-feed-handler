#pragma once
#include "../common/logging.hpp"
#include "moldudp64.hpp"

namespace itch {

class MoldUDP64Parser {
  public:
    struct ParseResult {
        bool valid = false;
        MoldUDP64Header header;
        const uint8_t* messages_begin = nullptr;
        std::size_t messages_length = 0;
    };

    static ParseResult parse(const uint8_t* data, std::size_t length);
};

} // namespace itch
