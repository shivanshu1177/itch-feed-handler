#pragma once
#include "../common/types.hpp"
#include "../network/packet.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace itch {

// -----------------------------------------------------------------
// RetransmitCredentials — venue login material, owned by FeedHandlerConfig.
// -----------------------------------------------------------------
struct RetransmitCredentials {
    std::string username;              // NASDAQ SoupBinTCP: max 6 chars on wire
    std::string password;              // NASDAQ SoupBinTCP: max 10 chars on wire
    bool        require_login{false};  // false → skip handshake (unauthenticated feeds)
};

// -----------------------------------------------------------------
// LoginHandler — abstract venue-specific login strategy.
//
// One subclass per venue (NASDAQ, SGX, CME, …).  Swap implementations
// via RetransmitClient::set_login_handler() before the first connect().
//
// The socket is still in BLOCKING mode when login() is called.
// Non-blocking mode is set by RetransmitClient after login succeeds.
// -----------------------------------------------------------------
class LoginHandler {
  public:
    virtual ~LoginHandler() = default;

    // Perform the full login handshake on a blocking TCP socket.
    //   fd:         connected blocking socket
    //   username:   null-terminated string (truncated to venue max on wire)
    //   password:   null-terminated string (truncated to venue max on wire)
    //   session_10: raw 10-byte MoldUDP64 session (NOT null-terminated);
    //               nullptr means "any session" (server chooses)
    // Returns true on acceptance; false on rejection or I/O error.
    // Implementations must log the specific failure reason.
    virtual bool login(int         fd,
                       const char* username,
                       const char* password,
                       const char* session_10) noexcept = 0;
};

// -----------------------------------------------------------------
// NasdaqLoginHandler — SoupBinTCP login for NASDAQ retransmit servers.
//
// Wire protocol (SoupBinTCP spec, §4):
//   Client → Server  Login Request  'L'  64-byte packet (see below)
//   Server → Client  Login Accepted 'A'  33 bytes — confirms session + seq
//                 or Login Rejected 'J'   4 bytes — reason: 'A'=unauthorized
//                                                            'S'=no such session
//
// Login Request wire layout (64 bytes total):
//   [0–1]   BE uint16   packet_length = 62  (excludes this 2-byte field)
//   [2]     char        packet_type   = 'L'
//   [3–8]   char[6]     username, right-padded with spaces
//   [9–18]  char[10]    password, right-padded with spaces
//   [19–28] char[10]    requested_session — spaces = "any"
//   [29–48] char[20]    requested_sequence — ASCII "1" left-justified, space-padded
//   [49–63] char[15]    reserved (zeroed)
// -----------------------------------------------------------------
class NasdaqLoginHandler final : public LoginHandler {
  public:
    bool login(int         fd,
               const char* username,
               const char* password,
               const char* session_10) noexcept override;

  private:
    enum class LoginResult { Accepted, Rejected, Error };

    // Reads the SoupBinTCP response with a 5-second SO_RCVTIMEO guard.
    // Clears the timeout before returning so the caller can set non-blocking.
    LoginResult recv_response(int fd) noexcept;
};

// -----------------------------------------------------------------
// RetransmitClient — sends MoldUDP64 retransmit requests via TCP.
//
// Lifecycle per gap:
//   1. connect()  — TCP connect primary → secondary; login if required
//   2. request()  — 20-byte MoldUDP64 retransmit request (non-blocking send)
//   3. recv()     — non-blocking drain of replayed UDP packets
//
// TLS readiness: raw_fd() exposes the socket descriptor so a future
// TLS P1 ticket can call SSL_set_fd(ssl, client.raw_fd()) without any
// other API change.  No OpenSSL headers are included here.
// -----------------------------------------------------------------
class RetransmitClient {
  public:
    RetransmitClient() = default;

    RetransmitClient(std::string           primary_addr,
                     uint16_t              primary_port,
                     std::string           secondary_addr  = {},
                     uint16_t              secondary_port  = 0,
                     RetransmitCredentials creds           = {}) noexcept;

    ~RetransmitClient() { disconnect(); }

    // Connect (primary → secondary fallback) then login if require_login.
    // If login is required but rejected: logs FATAL-level message and calls
    // exit(1) — a rejected login is a misconfiguration, not a transient fault.
    bool connect() noexcept;

    // Ad-hoc connect to an explicit endpoint; uses stored credentials.
    bool connect(const std::string& addr, uint16_t port) noexcept;

    // Send a retransmit request for [start_seq, start_seq+count).
    // session_10: raw 10-byte MoldUDP64 session (NOT null-terminated).
    bool request(const char* session_10, uint64_t start_seq, uint16_t count) noexcept;

    // Non-blocking receive. Returns bytes received, 0 if none, -1 on error.
    int recv(RawPacket& pkt) noexcept;

    void disconnect() noexcept;
    bool is_connected() const noexcept { return fd_ >= 0; }

    // Raw socket fd for future TLS integration — do not close directly.
    int raw_fd() const noexcept { return fd_; }

    // Replace the login handler before the first connect().
    // Default: NasdaqLoginHandler.  Swap for SgxLoginHandler etc.
    void set_login_handler(std::unique_ptr<LoginHandler> h) noexcept {
        login_handler_ = std::move(h);
    }

  private:
    int                           fd_{-1};
    std::string                   primary_addr_;
    uint16_t                      primary_port_{0};
    std::string                   secondary_addr_;
    uint16_t                      secondary_port_{0};
    RetransmitCredentials         creds_;
    std::unique_ptr<LoginHandler> login_handler_;

    // TCP connect + optional login for one specific endpoint.
    bool connect_to(const std::string& addr, uint16_t port) noexcept;
};

} // namespace itch
