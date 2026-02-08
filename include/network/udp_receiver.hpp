#pragma once
#include "packet.hpp"
#include <string>

namespace itch {

class UdpReceiver {
  public:
    virtual ~UdpReceiver() = default;
    virtual bool open(const std::string& addr, uint16_t port) = 0;
    virtual bool join_multicast(const std::string& group, const std::string& iface) = 0;
    virtual int recv(RawPacket& pkt) = 0; // Returns bytes received, 0 timeout, -1 error
    virtual void close() = 0;
};

} // namespace itch
