#pragma once
#include "../common/types.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../network/packet_ring.hpp"
#include "../protocol/sequence_tracker.hpp"
#include "../recovery/book_snapshot.hpp"
#include "../recovery/gap_detector.hpp"
#include "../recovery/retransmit_client.hpp"
#include "../system/cpu_affinity.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// RecoveryThread — handles gap detection and book snapshots.
//
// Runs on a dedicated core but is NOT on the critical latency path.
// Polls GapDetector, sends retransmit requests when gaps expire,
// and periodically saves BookSnapshots.
// -----------------------------------------------------------------
class RecoveryThread {
  public:
    RecoveryThread(GapDetector&                  gap_detector,
                   SequenceTracker&              seq_tracker,
                   ZeroCopyRing&                 inject_ring,  // re-inject retransmitted packets
                   const std::string&            retransmit_addr,
                   uint16_t                      retransmit_port,
                   const std::string&            retransmit_addr_secondary, // empty = disabled
                   uint16_t                      retransmit_port_secondary,
                   const RetransmitCredentials&  creds,
                   const std::string&            snapshot_dir,
                   const std::string&            snapshot_filename_template,
                   uint32_t                      snapshot_interval_sec,
                   const char*                   session,
                   FeedStats&                    stats,
                   const ThreadConfig&           cfg) noexcept;

    ~RecoveryThread() { stop(); }

    void start();
    void stop();

  private:
    GapDetector&      gap_detector_;
    SequenceTracker&  seq_tracker_;
    ZeroCopyRing&     inject_ring_;
    FeedStats&        stats_;
    BookSnapshot      snapshot_;
    uint32_t          snapshot_interval_sec_;
    char              session_[11]{};
    ThreadConfig      cfg_;

    RetransmitClient  retransmit_client_;
    std::atomic<bool> running_{false};
    std::thread       thread_;
    Timestamp         last_snapshot_ts_{0};

    void run() noexcept;
    void handle_recovery() noexcept;
    void recv_retransmitted() noexcept;
};

} // namespace itch
