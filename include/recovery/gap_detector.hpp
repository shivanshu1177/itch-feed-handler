#pragma once
#include "../common/types.hpp"
#include "../protocol/sequence_tracker.hpp"
#include <cstdint>

namespace itch {

// Describes a detected sequence gap (missing packet range)
struct GapRange {
    uint64_t  start_seq      = 0;
    uint64_t  end_seq        = 0;  // inclusive
    Timestamp detected_at_ns = 0;
    bool      recovery_sent  = false;
};

// -----------------------------------------------------------------
// GapDetector — tracks sequence continuity and manages recovery state.
//
// Used by the RecoveryThread which polls this on every heartbeat
// and after detecting gaps from the SequenceTracker.
// -----------------------------------------------------------------
class GapDetector {
  public:
    explicit GapDetector(uint64_t gap_timeout_ms = 500) noexcept
        : gap_timeout_ns_(gap_timeout_ms * 1'000'000ULL) {}

    // Called by RecoveryThread when SequenceTracker signals a gap.
    void on_gap(const GapInfo& info) noexcept;

    // Mark the gap as recovered up to (and including) seq.
    // Called by ParserThread when an in-order packet advances past gap_.end_seq.
    void mark_recovered(uint64_t up_to_seq) noexcept;

    // Mark that a retransmit request has been sent for the current gap.
    // Prevents RecoveryThread from re-requesting the same gap every poll cycle.
    void mark_recovery_sent() noexcept;

    // True if a gap is pending and timeout has elapsed (and not yet requested).
    bool should_request_recovery(Timestamp now_ns) const noexcept;

    bool     has_pending_gap()  const noexcept { return pending_; }
    GapRange pending_gap()      const noexcept { return gap_; }

  private:
    uint64_t gap_timeout_ns_;
    bool     pending_{false};
    GapRange gap_{};
};

} // namespace itch
