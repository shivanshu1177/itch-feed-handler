#pragma once
#include "../common/compiler.hpp"
#include "../common/types.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace itch {

// -----------------------------------------------------------------
// LatencyHistogram — lock-free, zero-allocation latency histogram.
//
// Layout (512 buckets total = 4KB, fits in L1 data cache):
//   Buckets 0–255:   linear, 1ns resolution, range [0, 255]ns
//   Buckets 256–511: log2 with 16 sub-buckets per power-of-two,
//                    covering [256ns, ~100ms]
//
// record() is ITCH_FORCE_INLINE — hot path cost: 1 atomic add.
// Percentile queries iterate all buckets — call only from reporter.
// -----------------------------------------------------------------
class LatencyHistogram {
  public:
    static constexpr uint32_t LINEAR_BUCKETS = 256;
    static constexpr uint32_t LOG_BUCKETS    = 256; // 16 sub-buckets × 16 octaves
    static constexpr uint32_t TOTAL_BUCKETS  = LINEAR_BUCKETS + LOG_BUCKETS;

    LatencyHistogram() = default;

    // Record one latency observation (nanoseconds).  Called on hot path.
    ITCH_FORCE_INLINE void record(uint64_t latency_ns) noexcept {
        uint32_t b = bucket_for(latency_ns);
        buckets_[b].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        // Update max via CAS (rare contention — reporter reads infrequently)
        uint64_t cur = max_.load(std::memory_order_relaxed);
        while (latency_ns > cur &&
               !max_.compare_exchange_weak(cur, latency_ns,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed))
            ;
    }

    uint64_t percentile(double p) const noexcept;
    uint64_t p50()  const noexcept { return percentile(50.0); }
    uint64_t p95()  const noexcept { return percentile(95.0); }
    uint64_t p99()  const noexcept { return percentile(99.0); }
    uint64_t p999() const noexcept { return percentile(99.9); }
    uint64_t max()  const noexcept { return max_.load(std::memory_order_relaxed); }
    uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }

    void reset() noexcept;

    void print_report(FILE* out, const char* name) const noexcept;

  private:
    alignas(64) std::atomic<uint64_t> buckets_[TOTAL_BUCKETS]{};
    alignas(64) std::atomic<uint64_t> count_{0};
    alignas(64) std::atomic<uint64_t> max_{0};

    // Map a latency value to a bucket index
    ITCH_FORCE_INLINE static uint32_t bucket_for(uint64_t ns) noexcept {
        if (ns < LINEAR_BUCKETS)
            return static_cast<uint32_t>(ns);

        // Log2 region: find which octave and sub-bucket
        int msb = 63 - __builtin_clzll(ns); // floor(log2(ns))
        if (msb >= 32)
            return TOTAL_BUCKETS - 1; // cap at max bucket

        // 16 sub-buckets per octave: use next 4 bits after MSB
        uint32_t sub = static_cast<uint32_t>(
            (ns >> (msb > 3 ? msb - 4 : 0)) & 0xF);
        uint32_t octave = static_cast<uint32_t>(msb - 7); // offset so octave 0 = [256,511]
        return LINEAR_BUCKETS + octave * 16 + sub;
    }

    // Lower bound of a log bucket in ns
    static uint64_t bucket_lower(uint32_t b) noexcept;
};

} // namespace itch
