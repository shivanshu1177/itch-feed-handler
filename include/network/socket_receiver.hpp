#pragma once
#include "udp_receiver.hpp"
#include <sys/socket.h>

namespace itch {

class SocketReceiver : public UdpReceiver {
  public:
    SocketReceiver();
    ~SocketReceiver() override;

    bool open(const std::string& addr, uint16_t port) override;
    bool join_multicast(const std::string& group, const std::string& iface) override;
    int recv(RawPacket& pkt) override;
    void close() override;

  private:
    int fd_ = -1;
};

} // namespace itch
