#include "recovery_thread.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/common/timestamp.hpp"
#include <chrono>
#include <cstring>

namespace itch {

RecoveryThread::RecoveryThread(GapDetector&                 gap_detector,
                                SequenceTracker&             seq_tracker,
                                ZeroCopyRing&                inject_ring,
                                const std::string&           retransmit_addr,
                                uint16_t                     retransmit_port,
                                const std::string&           retransmit_addr_secondary,
                                uint16_t                     retransmit_port_secondary,
                                const RetransmitCredentials& creds,
                                const std::string&           snapshot_dir,
                                const std::string&           snapshot_filename_template,
                                uint32_t                     snapshot_interval_sec,
                                const char*                  session,
                                FeedStats&                   stats,
                                const ThreadConfig&          cfg) noexcept
    : gap_detector_(gap_detector), seq_tracker_(seq_tracker),
      inject_ring_(inject_ring),
      stats_(stats),
      snapshot_(snapshot_dir, snapshot_filename_template),
      snapshot_interval_sec_(snapshot_interval_sec),
      retransmit_client_(retransmit_addr, retransmit_port,
                         retransmit_addr_secondary, retransmit_port_secondary,
                         creds),
      cfg_(cfg) {
    std::memset(session_, 0, sizeof(session_));
    if (session)
        std::memcpy(session_, session, std::min<std::size_t>(10, strlen(session)));
    ITCH_LOG_INFO("RecoveryThread: primary=%s:%u%s",
                  retransmit_addr.c_str(), retransmit_port,
                  retransmit_addr_secondary.empty() ? "" : " (secondary configured)");
}

void RecoveryThread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&RecoveryThread::run, this);
    ITCH_LOG_INFO("RecoveryThread started (cpu=%d)", cfg_.cpu_id);
}

void RecoveryThread::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
    retransmit_client_.disconnect();
}

void RecoveryThread::run() noexcept {
    CpuAffinity::apply(cfg_);
    last_snapshot_ts_ = steady_ns();

    while (running_.load(std::memory_order_acquire)) {
        // 1. Check for recovery-needed gaps
        handle_recovery();

        // 2. Drain any retransmitted packets back into the parser ring
        recv_retransmitted();

        // 3. Periodic snapshot (every snapshot_interval_sec_)
        Timestamp now = steady_ns();
        if ((now - last_snapshot_ts_) > static_cast<uint64_t>(snapshot_interval_sec_) * 1'000'000'000ULL) {
            // Books are owned by ShardWorkers — no direct access here yet.
            // Log the trigger; actual save is driven by FeedHandler when it
            // gains a book reference. session_ is used as the filename token.
            ITCH_LOG_INFO("RecoveryThread: snapshot interval reached (session='%.10s')",
                          session_);
            last_snapshot_ts_ = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RecoveryThread::handle_recovery() noexcept {
    if (!gap_detector_.has_pending_gap())
        return;

    Timestamp now = steady_ns();
    if (!gap_detector_.should_request_recovery(now))
        return;

    const GapRange& gap = gap_detector_.pending_gap();
    ITCH_LOG_WARN("RecoveryThread: requesting retransmit seq=[%lu,%lu]",
                  gap.start_seq, gap.end_seq);

    if (!retransmit_client_.is_connected()) {
        if (!retransmit_client_.connect()) {
            ITCH_LOG_ERROR("RecoveryThread: cannot connect to retransmit server "
                           "(primary and secondary both failed)");
            return;
        }
    }

    uint64_t count = gap.end_seq - gap.start_seq + 1;
    if (count > UINT16_MAX)
        count = UINT16_MAX; // MoldUDP64 max per request

    retransmit_client_.request(session_, gap.start_seq,
                                static_cast<uint16_t>(count));
    // Prevent re-requesting the same gap on every poll cycle.
    // GapDetector::should_request_recovery() returns false while recovery_sent
    // is true; it will be reset if a new (larger) gap is detected.
    gap_detector_.mark_recovery_sent();
}

void RecoveryThread::recv_retransmitted() noexcept {
    if (!retransmit_client_.is_connected())
        return;

    // Drain up to 64 packets per poll cycle so we don't monopolise the loop
    // when a large gap produces a burst of retransmitted data.
    for (int i = 0; i < 64; ++i) {
        RawPacket pkt;
        int n = retransmit_client_.recv(pkt);
        if (n <= 0)
            break; // EAGAIN or error — nothing more to read right now

        // Inject into the dedicated recovery SPSC in ZeroCopyRing.
        // ParserThread drains this queue at higher priority than the live
        // ring so gap healing is not blocked behind normal market data.
        inject_ring_.inject_recovered_packet(
            pkt.data,
            pkt.length,
            pkt.recv_timestamp_ns,
            stats_.recovery_drops,
            stats_.recovered_packets);
    }
}

} // namespace itch
