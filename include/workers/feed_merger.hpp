#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/timestamp.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../network/packet_ring.hpp"
#include "../system/cpu_affinity.hpp"
#include <atomic>
#include <cstdint>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// DedupWindow — lock-free sliding bitmask for sequence deduplication.
//
// Tracks which MoldUDP64 sequence numbers have already been forwarded.
// Window width = DEDUP_WINDOW_SIZE (128).  On A/B feeds that are
// microseconds apart the window never needs to advance.
//
// Single-threaded: only FeedMerger calls check_and_mark().
// -----------------------------------------------------------------
class DedupWindow {
  public:
    // Returns true if seq is NEW (not yet forwarded); marks it seen.
    // Returns false if seq is too old (< window base) or already seen.
    ITCH_FORCE_INLINE bool check_and_mark(uint64_t seq) noexcept {
        static_assert(DEDUP_WINDOW_SIZE % 64 == 0, "DEDUP_WINDOW_SIZE must be multiple of 64");
        static_assert(DEDUP_WINDOW_SIZE / 64 == 2, "This implementation hardcodes 2 words");

        if (ITCH_UNLIKELY(seq < base_))
            return false; // too old to track — already forwarded

        // Advance window until seq fits.  Each step shifts one 64-seq block.
        while (ITCH_UNLIKELY(seq >= base_ + DEDUP_WINDOW_SIZE)) {
            bits_[0] = bits_[1];
            bits_[1] = 0;
            base_ += 64;
        }

        uint64_t offset = seq - base_;
        uint64_t word   = offset >> 6;   // / 64
        uint64_t mask   = uint64_t{1} << (offset & 63);

        if (bits_[word] & mask)
            return false; // duplicate
        bits_[word] |= mask;
        return true;
    }

    void reset() noexcept {
        base_    = 0;
        bits_[0] = 0;
        bits_[1] = 0;
    }

  private:
    uint64_t base_{0};
    uint64_t bits_[DEDUP_WINDOW_SIZE / 64]{};
};

// -----------------------------------------------------------------
// FeedMerger — merges primary and secondary A/B multicast feeds.
//
// Thread model:
//   Producers: ReceiverThread (primary → primary_ring_)
//              ReceiverThread (secondary → secondary_ring_)
//              RecoveryThread (→ primary_ring_.recovery_queue)
//   Consumer of both rings: FeedMerger (this class)
//   Producer of merged_queue_: FeedMerger
//   Consumer of merged_queue_: ParserThread
//
// Hot-path invariants:
//   • No memory allocation.
//   • No locks — all communication via SPSC queues.
//   • One branch per slot for dedup (ITCH_UNLIKELY when dup rate is low).
//   • Failover check every 4096 slots (amortised single comparison).
//
// Secondary feed is optional.  If secondary_ring_.buffer is nullptr,
// the merger acts as a thin forwarding shim with zero extra copies.
// -----------------------------------------------------------------
class FeedMerger {
  public:
    FeedMerger(ZeroCopyRing&    primary_ring,
               ZeroCopyRing&    secondary_ring,   // buffer may be nullptr if disabled
               MergedSlotQueue& merged_queue,
               FeedStats&       stats,
               uint64_t         failover_timeout_ms,
               const ThreadConfig& cfg) noexcept
        : primary_ring_(primary_ring),
          secondary_ring_(secondary_ring),
          merged_queue_(merged_queue),
          stats_(stats),
          failover_timeout_ns_(failover_timeout_ms * 1'000'000ULL),
          cfg_(cfg) {}

    ~FeedMerger() { stop(); }

    void start();
    void stop();

    bool is_failed_over() const noexcept { return failed_over_; }

  private:
    ZeroCopyRing&    primary_ring_;
    ZeroCopyRing&    secondary_ring_;
    MergedSlotQueue& merged_queue_;
    FeedStats&       stats_;
    uint64_t         failover_timeout_ns_;
    ThreadConfig     cfg_;

    DedupWindow dedup_;
    Timestamp   primary_last_ts_{0};
    Timestamp   last_failover_log_ts_{0};
    bool        failed_over_{false};
    bool        secondary_enabled_{false}; // set in run() based on ring buffer presence

    // Amortise failover check: only call steady_ns() every N slots.
    static constexpr uint32_t FAILOVER_CHECK_INTERVAL = 4096;
    uint32_t slot_counter_{0};

    std::atomic<bool> running_{false};
    std::thread       thread_;

    void run() noexcept;

    // Decode MoldUDP64 sequence number from slot data (big-endian at byte 10).
    // Returns 0 on short/invalid packets (caller should always forward those).
    ITCH_FORCE_INLINE uint64_t peek_sequence(const PacketSlot* s) const noexcept {
        if (ITCH_UNLIKELY(s->length < 20))
            return 0; // shorter than MoldUDP64 header — forward unconditionally
        uint64_t seq;
        __builtin_memcpy(&seq, s->data + 10, 8);
        return __builtin_bswap64(seq);
    }

    // Peek message count (big-endian uint16 at byte 18).
    ITCH_FORCE_INLINE uint16_t peek_msg_count(const PacketSlot* s) const noexcept {
        uint16_t mc;
        __builtin_memcpy(&mc, s->data + 18, 2);
        return __builtin_bswap16(mc);
    }

    // Attempt to forward ms to ParserThread.  Drops (with counter) on queue full.
    ITCH_FORCE_INLINE void forward(MergedSlot ms) noexcept {
        if (ITCH_UNLIKELY(!merged_queue_.try_push(ms)))
            stats_.merged_drops.fetch_add(1, std::memory_order_relaxed);
    }

    // Handle a slot from one of the live rings (not recovery).
    // Performs dedup; increments secondary counter; calls forward().
    void handle_live_slot(uint16_t slot_idx, FeedSource source) noexcept;

    // Check primary feed health and log failover events.
    void check_failover() noexcept;
};

} // namespace itch
