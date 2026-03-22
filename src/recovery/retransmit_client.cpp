#include "retransmit_client.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/common/timestamp.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace itch {

// ---------------------------------------------------------------------------
// Internal I/O helpers (used only by NasdaqLoginHandler — not on hot path)
// ---------------------------------------------------------------------------

// Blocking write of exactly `count` bytes; retries on EINTR.
static bool write_all(int fd, const void* buf, std::size_t count) noexcept {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (count > 0) {
        ssize_t n = ::send(fd, p, count, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p     += static_cast<std::size_t>(n);
        count -= static_cast<std::size_t>(n);
    }
    return true;
}

// Blocking read of exactly `count` bytes; retries on EINTR.
static bool read_all(int fd, void* buf, std::size_t count) noexcept {
    auto* p = static_cast<uint8_t*>(buf);
    while (count > 0) {
        ssize_t n = ::recv(fd, p, count, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; // peer closed
        p     += static_cast<std::size_t>(n);
        count -= static_cast<std::size_t>(n);
    }
    return true;
}

// Copy at most dst_len bytes from src; pad remainder with fill_char.
static void pad_copy(char* dst, std::size_t dst_len,
                      const char* src, char fill_char = ' ') noexcept {
    std::size_t src_len = (src && *src) ? std::strlen(src) : 0;
    std::size_t n       = src_len < dst_len ? src_len : dst_len;
    if (n) std::memcpy(dst, src, n);
    if (dst_len > n) std::memset(dst + n, fill_char, dst_len - n);
}

// ---------------------------------------------------------------------------
// NasdaqLoginHandler
// ---------------------------------------------------------------------------

bool NasdaqLoginHandler::login(int fd, const char* username,
                                const char* password,
                                const char* session_10) noexcept {
    // Build the 64-byte SoupBinTCP Login Request.
    uint8_t pkt[64]{};  // zero-initialised — reserved bytes stay zero

    // Packet length (BE): covers everything after this 2-byte field.
    pkt[0] = 0x00;
    pkt[1] = 62;   // 64 - 2

    pkt[2] = 'L';  // packet type

    pad_copy(reinterpret_cast<char*>(pkt + 3),  6,  username);  // username[6]
    pad_copy(reinterpret_cast<char*>(pkt + 9),  10, password);  // password[10]

    // Requested session [19–28]: copy the raw 10-byte wire field, or spaces
    // if nullptr ("any session" — server will pick the active session).
    if (session_10)
        std::memcpy(pkt + 19, session_10, 10);
    else
        std::memset(pkt + 19, ' ', 10);

    // Requested sequence [29–48]: ASCII "1" left-justified, space-padded.
    // Asking for seq 1 lets the server decide; recovery logic sends explicit
    // retransmit requests afterwards anyway.
    pkt[29] = '1';
    std::memset(pkt + 30, ' ', 19);

    // [49–63] reserved — already zero from value-init above.

    if (!write_all(fd, pkt, sizeof(pkt))) {
        ITCH_LOG_ERROR("NasdaqLoginHandler: failed to send login request: %s",
                       strerror(errno));
        return false;
    }
    ITCH_LOG_INFO("NasdaqLoginHandler: login request sent (user='%.6s')",
                  username ? username : "");

    return recv_response(fd) == LoginResult::Accepted;
}

NasdaqLoginHandler::LoginResult
NasdaqLoginHandler::recv_response(int fd) noexcept {
    // Set a 5-second receive timeout for the login phase only.
    // Cleared before return so the caller can safely set O_NONBLOCK.
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto clear_timeout = [&]() noexcept {
        tv = {0, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    };

    // SoupBinTCP: 2-byte BE length prefix, then payload.
    uint8_t len_buf[2];
    if (!read_all(fd, len_buf, 2)) {
        ITCH_LOG_ERROR("NasdaqLoginHandler: failed to read response length: %s",
                       strerror(errno));
        clear_timeout();
        return LoginResult::Error;
    }

    uint16_t payload_len =
        static_cast<uint16_t>((static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1]);
    if (payload_len == 0 || payload_len > 64) {
        ITCH_LOG_ERROR("NasdaqLoginHandler: implausible response length %u", payload_len);
        clear_timeout();
        return LoginResult::Error;
    }

    uint8_t payload[64]{};
    if (!read_all(fd, payload, payload_len)) {
        ITCH_LOG_ERROR("NasdaqLoginHandler: failed to read response payload: %s",
                       strerror(errno));
        clear_timeout();
        return LoginResult::Error;
    }
    clear_timeout();

    char pkt_type = static_cast<char>(payload[0]);

    if (pkt_type == 'A') {
        // Login Accepted: payload[1..10]=session, payload[11..30]=seq (ASCII)
        char session[11]{};
        std::size_t copy = payload_len > 1
            ? (payload_len - 1 < 10 ? payload_len - 1 : 10) : 0;
        std::memcpy(session, payload + 1, copy);
        ITCH_LOG_INFO("NasdaqLoginHandler: login accepted, server session='%.10s'",
                      session);
        return LoginResult::Accepted;
    }

    if (pkt_type == 'J') {
        char reason      = (payload_len > 1) ? static_cast<char>(payload[1]) : '?';
        const char* desc = (reason == 'A') ? "Not Authorized" :
                           (reason == 'S') ? "Session Not Available" : "Unknown";
        ITCH_LOG_ERROR("NasdaqLoginHandler: login REJECTED — '%c' (%s)", reason, desc);
        return LoginResult::Rejected;
    }

    ITCH_LOG_ERROR("NasdaqLoginHandler: unexpected response type '0x%02x'",
                   static_cast<unsigned>(pkt_type));
    return LoginResult::Error;
}

// ---------------------------------------------------------------------------
// RetransmitClient
// ---------------------------------------------------------------------------

RetransmitClient::RetransmitClient(std::string           primary_addr,
                                    uint16_t              primary_port,
                                    std::string           secondary_addr,
                                    uint16_t              secondary_port,
                                    RetransmitCredentials creds) noexcept
    : primary_addr_(std::move(primary_addr)),
      primary_port_(primary_port),
      secondary_addr_(std::move(secondary_addr)),
      secondary_port_(secondary_port),
      creds_(std::move(creds)),
      login_handler_(std::make_unique<NasdaqLoginHandler>()) {}

bool RetransmitClient::connect_to(const std::string& addr, uint16_t port) noexcept {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        ITCH_LOG_ERROR("RetransmitClient: socket failed: %s", strerror(errno));
        return false;
    }

    // Disable Nagle — the 20-byte request must go out immediately.
    int nodelay = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
        ITCH_LOG_ERROR("RetransmitClient: invalid address '%s'", addr.c_str());
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Blocking connect — socket is still blocking here; required for login.
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
        ITCH_LOG_ERROR("RetransmitClient: connect to %s:%u failed: %s",
                       addr.c_str(), port, strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    ITCH_LOG_INFO("RetransmitClient: TCP connected to %s:%u", addr.c_str(), port);

    // Login handshake while socket is still blocking — simpler I/O loops.
    if (creds_.require_login) {
        if (!login_handler_ ||
            !login_handler_->login(fd_,
                                   creds_.username.c_str(),
                                   creds_.password.c_str(),
                                   nullptr /* any session */)) {
            // A rejected login is a misconfiguration, not a transient fault.
            // Log at FATAL level and exit so operations is paged immediately.
            fprintf(stderr,
                    "[FATAL] RetransmitClient: login to %s:%u failed. "
                    "Check [recovery].retransmit_username / retransmit_password "
                    "and venue firewall rules.\n",
                    addr.c_str(), port);
            ::close(fd_);
            fd_ = -1;
            exit(1);
        }
    }

    // Switch to non-blocking AFTER login completes.  Non-blocking mode is
    // correct for the hot-path recv() poll in RecoveryThread::recv_retransmitted().
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    ITCH_LOG_INFO("RetransmitClient: ready on %s:%u (login=%s)",
                  addr.c_str(), port, creds_.require_login ? "yes" : "no");
    return true;
}

bool RetransmitClient::connect() noexcept {
    if (connect_to(primary_addr_, primary_port_))
        return true;

    if (!secondary_addr_.empty() && secondary_port_ != 0) {
        ITCH_LOG_WARN("RetransmitClient: primary failed, trying secondary %s:%u",
                      secondary_addr_.c_str(), secondary_port_);
        return connect_to(secondary_addr_, secondary_port_);
    }
    return false;
}

bool RetransmitClient::connect(const std::string& addr, uint16_t port) noexcept {
    return connect_to(addr, port);
}

bool RetransmitClient::request(const char* session_10, uint64_t start_seq,
                                uint16_t count) noexcept {
    if (!is_connected())
        return false;

    // MoldUDP64 retransmit request: 20 bytes
    //   session[10] + sequence[8 BE] + count[2 BE]
    uint8_t buf[20];
    std::memcpy(buf, session_10, 10);

    uint64_t seq = start_seq;  // preserve for logging before loop shifts it out
    for (int i = 7; i >= 0; --i) {
        buf[10 + i] = static_cast<uint8_t>(seq & 0xFF);
        seq >>= 8;
    }
    buf[18] = static_cast<uint8_t>((count >> 8) & 0xFF);
    buf[19] = static_cast<uint8_t>(count & 0xFF);

    ssize_t n = ::send(fd_, buf, sizeof(buf), MSG_NOSIGNAL);
    if (n != static_cast<ssize_t>(sizeof(buf))) {
        ITCH_LOG_ERROR("RetransmitClient: send request failed: %s", strerror(errno));
        return false;
    }

    ITCH_LOG_INFO("RetransmitClient: requested seq=%lu count=%u", start_seq, count);
    return true;
}

int RetransmitClient::recv(RawPacket& pkt) noexcept {
    if (!is_connected())
        return -1;

    ssize_t n = ::recv(fd_, pkt.data, sizeof(pkt.data), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        ITCH_LOG_ERROR("RetransmitClient: recv error: %s", strerror(errno));
        return -1;
    }
    if (n == 0) {
        ITCH_LOG_WARN("RetransmitClient: server closed connection");
        disconnect();
        return -1;
    }
    pkt.length            = static_cast<uint16_t>(n);
    pkt.recv_timestamp_ns = now_ns();
    return static_cast<int>(n);
}

void RetransmitClient::disconnect() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        ITCH_LOG_INFO("RetransmitClient: disconnected");
    }
}

} // namespace itch
