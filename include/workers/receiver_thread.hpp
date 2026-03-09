#pragma once
#include "../common/types.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../network/packet_ring.hpp"
#include "../network/socket_receiver.hpp"
#include "../system/cpu_affinity.hpp"
#include <atomic>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// ReceiverThread — hot-loop UDP receiver.
//
// Tight spin on recvfrom/recvmsg → writes directly into the zero-
// copy ring buffer → publishes slot index. No allocation, no locks.
// Pinned to cpu_affinity.cpu_id with optional SCHED_FIFO.
// -----------------------------------------------------------------
class ReceiverThread {
  public:
    ReceiverThread(ZeroCopyRing& ring, FeedStats& stats,
                   const ThreadConfig& cfg) noexcept
        : ring_(ring), stats_(stats), cfg_(cfg) {}

    ~ReceiverThread() { stop(); }

    bool open(const std::string& addr, uint16_t port,
              const std::string& mcast_group,
              const std::string& mcast_iface) noexcept;

    void start();
    void stop();

    uint64_t pkts_received() const noexcept { return pkts_received_; }
    uint64_t pkts_dropped()  const noexcept { return pkts_dropped_; }

  private:
    ZeroCopyRing&   ring_;
    FeedStats&      stats_;
    ThreadConfig    cfg_;
    SocketReceiver  sock_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    uint64_t pkts_received_{0};
    uint64_t pkts_dropped_{0};
    uint16_t write_slot_{0};

    void run() noexcept;
};

} // namespace itch
