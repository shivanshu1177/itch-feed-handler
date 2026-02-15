#include "sequence_tracker.hpp"
#include "../common/logging.hpp"
#include "../common/timestamp.hpp"

namespace itch {

bool SequenceTracker::try_advance(uint64_t next_seq) noexcept {
    uint64_t current = seq_.load(std::memory_order_relaxed);
    uint64_t expected = current + 1;

    if (ITCH_LIKELY(next_seq == expected)) {
        seq_.store(next_seq, std::memory_order_release);
        return true;
    }

    if (next_seq > expected) {
        // Gap detected: sequences [expected, next_seq-1] are missing
        uint64_t gap_size = next_seq - expected;
        gap_count_.fetch_add(1, std::memory_order_relaxed);

        Timestamp now = steady_ns();
        last_gap_start_.store(expected,  std::memory_order_relaxed);
        last_gap_end_.store(next_seq - 1, std::memory_order_relaxed);
        last_gap_ts_.store(now,           std::memory_order_relaxed);

        ITCH_LOG_WARN("Sequence gap: expected=%lu got=%lu gap_size=%lu",
                      expected, next_seq, gap_size);

        // Accept the jump so processing can continue; recovery thread will
        // request retransmission of the missing range
        seq_.store(next_seq, std::memory_order_release);
        return false;
    }

    // next_seq <= current: duplicate or reordered packet, silently discard
    dup_count_.fetch_add(1, std::memory_order_relaxed);
    ITCH_LOG_DEBUG("Duplicate/reordered sequence: got=%lu current=%lu", next_seq, current);
    return false;
}

void SequenceTracker::reset(uint64_t to) noexcept {
    seq_.store(to, std::memory_order_release);
    gap_count_.store(0, std::memory_order_relaxed);
    dup_count_.store(0, std::memory_order_relaxed);
    last_gap_start_.store(0, std::memory_order_relaxed);
    last_gap_end_.store(0, std::memory_order_relaxed);
    last_gap_ts_.store(0, std::memory_order_relaxed);
    ITCH_LOG_INFO("SequenceTracker reset to %lu", to);
}

GapInfo SequenceTracker::last_gap() const noexcept {
    GapInfo g;
    g.start_seq      = last_gap_start_.load(std::memory_order_relaxed);
    g.end_seq        = last_gap_end_.load(std::memory_order_relaxed);
    g.detected_at_ns = last_gap_ts_.load(std::memory_order_relaxed);
    return g;
}

} // namespace itch
