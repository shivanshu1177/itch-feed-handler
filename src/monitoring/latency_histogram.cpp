#include "latency_histogram.hpp"
#include <cstring>

namespace itch {

uint64_t LatencyHistogram::bucket_lower(uint32_t b) noexcept {
    if (b < LINEAR_BUCKETS)
        return b; // each linear bucket represents exactly b ns
    uint32_t log_b  = b - LINEAR_BUCKETS;
    uint32_t octave = log_b / 16;
    uint32_t sub    = log_b % 16;
    uint32_t msb    = octave + 7; // msb of the lower bound
    return (static_cast<uint64_t>(1) << msb) | (static_cast<uint64_t>(sub) << (msb - 4));
}

uint64_t LatencyHistogram::percentile(double p) const noexcept {
    uint64_t total = count_.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;

    uint64_t target = static_cast<uint64_t>(total * p / 100.0);
    uint64_t cumulative = 0;

    for (uint32_t i = 0; i < TOTAL_BUCKETS; ++i) {
        cumulative += buckets_[i].load(std::memory_order_relaxed);
        if (cumulative > target)
            return bucket_lower(i);
    }
    return max_.load(std::memory_order_relaxed);
}

void LatencyHistogram::reset() noexcept {
    for (auto& b : buckets_)
        b.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
}

void LatencyHistogram::print_report(FILE* out, const char* name) const noexcept {
    uint64_t n = count_.load(std::memory_order_relaxed);
    if (n == 0) {
        fprintf(out, "%-30s  no samples\n", name);
        return;
    }
    fprintf(out, "%-30s  n=%-10lu  p50=%-8lu  p95=%-8lu  p99=%-8lu  p99.9=%-8lu  max=%-8lu  (ns)\n",
            name, n, p50(), p95(), p99(), p999(), max());
}

} // namespace itch
