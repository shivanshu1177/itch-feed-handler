#pragma once
#include "../common/types.hpp"
#include <atomic>
#include <cstdint>

namespace itch {

struct GapInfo {
    uint64_t start_seq = 0;  // first missing sequence number
    uint64_t end_seq   = 0;  // last missing sequence number (inclusive)
    Timestamp detected_at_ns = 0;
};

// Thread-safe sequence number tracker with gap and duplicate detection.
// Designed for single-writer (parser thread) / single-reader (recovery thread).
class SequenceTracker {
  public:
    explicit SequenceTracker(uint64_t initial = 0) : seq_(initial) {}

    // Call for every received sequence number.
    // Returns true  → packet is in-order (no gap before it)
    // Returns false → gap detected OR duplicate; check gap_count() to distinguish
    bool try_advance(uint64_t next_seq) noexcept;

    uint64_t current() const noexcept { return seq_.load(std::memory_order_acquire); }

    uint64_t gap_count()  const noexcept { return gap_count_.load(std::memory_order_relaxed); }
    uint64_t dup_count()  const noexcept { return dup_count_.load(std::memory_order_relaxed); }
    GapInfo  last_gap()   const noexcept;

    // Reset to a known sequence (called by SessionManager on session change).
    void reset(uint64_t to = 0) noexcept;

  private:
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> gap_count_{0};
    std::atomic<uint64_t> dup_count_{0};
    // Last gap is written only by parser thread — relaxed store is fine
    std::atomic<uint64_t> last_gap_start_{0};
    std::atomic<uint64_t> last_gap_end_{0};
    std::atomic<uint64_t> last_gap_ts_{0};
};

} // namespace itch
