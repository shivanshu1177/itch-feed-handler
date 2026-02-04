#include "socket_receiver.hpp"
#include "../common/logging.hpp"
#include "../common/timestamp.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef SO_TIMESTAMPING
#include <linux/net_tstamp.h>
#endif

namespace itch {

SocketReceiver::SocketReceiver() = default;

SocketReceiver::~SocketReceiver() {
    close();
}

bool SocketReceiver::open(const std::string& addr, uint16_t port) {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        ITCH_LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return false;
    }

    int reuse = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ITCH_LOG_WARN("Failed to set SO_REUSEADDR");
    }

#ifdef SO_REUSEPORT
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        ITCH_LOG_WARN("Failed to set SO_REUSEPORT");
    }
#endif

    int rcvbuf = 8 * 1024 * 1024;
    if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        ITCH_LOG_WARN("Failed to set SO_RCVBUF");
    }

    // Enable NIC hardware timestamps on Linux for minimum latency measurement jitter
#ifdef SO_TIMESTAMPING
    int ts_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    if (setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags)) < 0) {
        ITCH_LOG_WARN("SO_TIMESTAMPING not available, using software timestamps");
    } else {
        ITCH_LOG_INFO("NIC hardware timestamps enabled");
    }
#endif

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);

    if (bind(fd_, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        ITCH_LOG_ERROR("Bind failed: %s", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    ITCH_LOG_INFO("Socket opened on %s:%u", addr.c_str(), port);
    return true;
}

bool SocketReceiver::join_multicast(const std::string& group, const std::string& iface) {
    if (fd_ < 0)
        return false;

    struct ip_mreq mreq{};
    inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = inet_addr(iface.c_str());

    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ITCH_LOG_ERROR("Multicast join failed: %s", strerror(errno));
        return false;
    }

    ITCH_LOG_INFO("Joined multicast %s on %s", group.c_str(), iface.c_str());
    return true;
}

int SocketReceiver::recv(RawPacket& pkt) {
#ifdef SO_TIMESTAMPING
    // Use recvmsg to extract NIC hardware timestamps from cmsg
    struct iovec iov;
    iov.iov_base = pkt.data;
    iov.iov_len = sizeof(pkt.data);

    // Control message buffer large enough for scm_timestamping
    alignas(16) char ctrl_buf[256];
    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    ssize_t n = recvmsg(fd_, &msg, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        ITCH_LOG_ERROR("recvmsg error: %s", strerror(errno));
        return -1;
    }

    pkt.length = static_cast<uint16_t>(n);
    pkt.recv_timestamp_ns = now_ns(); // software fallback

    // Walk cmsgs looking for SCM_TIMESTAMPING (hardware ts is in ts[2])
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
            const struct timespec* ts = (const struct timespec*)CMSG_DATA(cmsg);
            // ts[0] = SW, ts[1] = deprecated, ts[2] = HW raw
            if (ts[2].tv_sec != 0 || ts[2].tv_nsec != 0) {
                pkt.recv_timestamp_ns =
                    static_cast<uint64_t>(ts[2].tv_sec) * 1'000'000'000ULL +
                    static_cast<uint64_t>(ts[2].tv_nsec);
            }
            break;
        }
    }
    return static_cast<int>(n);
#else
    struct sockaddr_in sender{};
    socklen_t sender_len = sizeof(sender);
    ssize_t n = recvfrom(fd_, pkt.data, sizeof(pkt.data), 0,
                         (struct sockaddr*)&sender, &sender_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        ITCH_LOG_ERROR("recvfrom error: %s", strerror(errno));
        return -1;
    }

    pkt.length = static_cast<uint16_t>(n);
    pkt.recv_timestamp_ns = now_ns();
    return static_cast<int>(n);
#endif
}

void SocketReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        ITCH_LOG_INFO("Socket closed");
    }
}

} // namespace itch
