#include "feed_merger.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/common/timestamp.hpp"

namespace itch {

void FeedMerger::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&FeedMerger::run, this);
    ITCH_LOG_INFO("FeedMerger started (cpu=%d, secondary=%s)",
                  cfg_.cpu_id,
                  secondary_enabled_ ? "enabled" : "disabled");
}

void FeedMerger::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

void FeedMerger::run() noexcept {
    CpuAffinity::apply(cfg_);

    secondary_enabled_ = (secondary_ring_.buffer != nullptr);
    primary_last_ts_   = steady_ns(); // initialise so failover doesn't fire at startup

    while (running_.load(std::memory_order_acquire)) {
        // 1. Recovery (highest priority) — drain all available before live packets.
        //    RecoveryThread injects into primary_ring_.recovery_queue.
        uint16_t rec_idx;
        if (primary_ring_.consume_recovered(rec_idx)) {
            forward({rec_idx, FeedSource::Recovery});
            continue; // stay in recovery drain until queue empties
        }

        // 2. Live feeds — check both rings, dedup, forward.
        uint16_t pidx;
        bool got_p = primary_ring_.consume(pidx);
        if (got_p)
            handle_live_slot(pidx, FeedSource::Primary);

        if (secondary_enabled_) {
            uint16_t sidx;
            bool got_s = secondary_ring_.consume(sidx);
            if (got_s)
                handle_live_slot(sidx, FeedSource::Secondary);
        }

        // 3. Failover health check (amortised — runs every FAILOVER_CHECK_INTERVAL slots)
        if (ITCH_UNLIKELY(++slot_counter_ >= FAILOVER_CHECK_INTERVAL)) {
            slot_counter_ = 0;
            if (secondary_enabled_)
                check_failover();
        }
    }
}

void FeedMerger::handle_live_slot(uint16_t slot_idx, FeedSource source) noexcept {
    const PacketSlot* s = (source == FeedSource::Primary)
        ? primary_ring_.slot(slot_idx)
        : secondary_ring_.slot(slot_idx);

    if (source == FeedSource::Primary) {
        primary_last_ts_ = s->recv_ts ? s->recv_ts : steady_ns();
        if (ITCH_UNLIKELY(failed_over_)) {
            ITCH_LOG_INFO("FeedMerger: primary feed restored after failover");
            failed_over_ = false;
        }
    } else {
        stats_.secondary_packets.fetch_add(1, std::memory_order_relaxed);
    }

    // Short packet or invalid header: forward unconditionally (no dedup possible).
    uint64_t seq = peek_sequence(s);
    if (ITCH_UNLIKELY(seq == 0)) {
        forward({slot_idx, source});
        return;
    }

    // Heartbeats (message_count == 0xFFFF): always forward, skip dedup.
    // Heartbeats from both feeds keep the gap detector alive and the
    // connection alive; both should reach ParserThread.
    if (ITCH_UNLIKELY(peek_msg_count(s) == MOLDUDP64_HEARTBEAT)) {
        forward({slot_idx, source});
        return;
    }

    // Dedup: drop if this sequence number was already forwarded.
    if (ITCH_UNLIKELY(!dedup_.check_and_mark(seq))) {
        stats_.dedup_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    forward({slot_idx, source});
}

void FeedMerger::check_failover() noexcept {
    Timestamp now = steady_ns();

    if (!failed_over_) {
        if (ITCH_UNLIKELY(primary_last_ts_ > 0 &&
                          (now - primary_last_ts_) > failover_timeout_ns_)) {
            failed_over_ = true;
            last_failover_log_ts_ = now;
            stats_.feed_failover_count.fetch_add(1, std::memory_order_relaxed);
            ITCH_LOG_WARN("FeedMerger: primary feed silent for >%llums — running on secondary only",
                          static_cast<unsigned long long>(failover_timeout_ns_ / 1'000'000ULL));
        }
    } else {
        // Log once per second while in failover state (avoid log storm)
        if ((now - last_failover_log_ts_) >= 1'000'000'000ULL) {
            last_failover_log_ts_ = now;
            ITCH_LOG_WARN("FeedMerger: still in failover — primary has been silent for %llus",
                          static_cast<unsigned long long>((now - primary_last_ts_) / 1'000'000'000ULL));
        }
    }
}

} // namespace itch
