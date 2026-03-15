#include "metrics_reporter.hpp"
#include "../../include/common/constants.hpp"
#include "../../include/common/logging.hpp"
#include <chrono>
#include <cstdio>

namespace itch {

MetricsReporter::MetricsReporter(uint32_t report_interval_sec,
                                 const FeedStats& stats,
                                 LatencyHistogram* parse_lat,
                                 LatencyHistogram* book_lat_shards,
                                 uint32_t num_shards)
    : interval_sec_(report_interval_sec),
      stats_(stats),
      parse_lat_(parse_lat),
      book_lat_shards_(book_lat_shards),
      num_shards_(num_shards) {}

MetricsReporter::~MetricsReporter() { stop(); }

void MetricsReporter::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&MetricsReporter::run, this);
    ITCH_LOG_INFO("MetricsReporter started (interval=%us)", interval_sec_);
}

void MetricsReporter::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

void MetricsReporter::run() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_sec_));
        if (!running_.load(std::memory_order_acquire))
            break;
        print_report();
    }
}

void MetricsReporter::print_report() noexcept {
    FILE* out = stderr;
    fprintf(out, "\n========== Feed Handler Metrics ==========\n");

    // Throughput counters
    fprintf(out, "Packets    received=%-12lu dropped=%-12lu\n",
            stats_.packets_received.load(std::memory_order_relaxed),
            stats_.packets_dropped.load(std::memory_order_relaxed));
    fprintf(out, "Messages   parsed=%-14lu\n",
            stats_.messages_parsed.load(std::memory_order_relaxed));
    fprintf(out, "Sequence   gaps=%-16lu dups=%-14lu\n",
            stats_.sequence_gaps.load(std::memory_order_relaxed),
            stats_.sequence_dups.load(std::memory_order_relaxed));
    fprintf(out, "Recovery   recovered=%-11lu drops=%-14lu\n",
            stats_.recovered_packets.load(std::memory_order_relaxed),
            stats_.recovery_drops.load(std::memory_order_relaxed));
    fprintf(out, "FeedMerger secondary=%-12lu dedup_drops=%-9lu failovers=%-10lu",
            stats_.secondary_packets.load(std::memory_order_relaxed),
            stats_.dedup_drops.load(std::memory_order_relaxed),
            stats_.feed_failover_count.load(std::memory_order_relaxed));
    uint64_t mdrops = stats_.merged_drops.load(std::memory_order_relaxed);
    if (mdrops > 0)
        fprintf(out, " merged_drops=%lu -- WARNING: merged queue full\n", mdrops);
    else
        fprintf(out, "\n");

    // In-place replace counter (same-price + qty-reduced; time priority preserved)
    uint64_t total_inplace = 0;
    for (uint32_t i = 0; i < num_shards_; ++i)
        total_inplace += stats_.order_replaces_inplace[i].load(std::memory_order_relaxed);
    fprintf(out, "Replaces   inplace=%-14lu (same-price qty-reduced; priority preserved)\n",
            total_inplace);

    // Per-shard queue drop counters
    uint64_t total_queue_drops = 0;
    for (uint32_t i = 0; i < num_shards_; ++i)
        total_queue_drops += stats_.shard_queue_drops[i].load(std::memory_order_relaxed);
    if (total_queue_drops > 0) {
        fprintf(out, "ShardDrops total=%-13lu [", total_queue_drops);
        for (uint32_t i = 0; i < num_shards_; ++i) {
            fprintf(out, "%lu%s",
                    stats_.shard_queue_drops[i].load(std::memory_order_relaxed),
                    i + 1 < num_shards_ ? " " : "");
        }
        fprintf(out, "]\n");
    } else {
        fprintf(out, "ShardDrops total=0\n");
    }

    // Per-shard price-level overflow counters
    // Printed unconditionally when non-zero; suggested action included.
    uint64_t total_lvl_drops = 0;
    for (uint32_t i = 0; i < num_shards_; ++i)
        total_lvl_drops += stats_.price_level_drops[i].load(std::memory_order_relaxed);
    if (total_lvl_drops > 0) {
        fprintf(out, "PriceLevelDrops total=%-9lu [", total_lvl_drops);
        for (uint32_t i = 0; i < num_shards_; ++i) {
            fprintf(out, "%lu%s",
                    stats_.price_level_drops[i].load(std::memory_order_relaxed),
                    i + 1 < num_shards_ ? " " : "");
        }
        fprintf(out, "] -- WARNING: book depth exceeded MAX_PRICE_LEVELS=%u; "
                     "increase constexpr MAX_PRICE_LEVELS in constants.hpp and rebuild\n",
                static_cast<unsigned>(MAX_PRICE_LEVELS));
    }

    // Latency histograms
    fprintf(out, "\nLatency (wire-to-parse):\n");
    if (parse_lat_)
        parse_lat_->print_report(out, "  parse");

    fprintf(out, "\nLatency (wire-to-book) per shard:\n");
    if (book_lat_shards_) {
        for (uint32_t i = 0; i < num_shards_; ++i) {
            char label[32];
            snprintf(label, sizeof(label), "  shard[%u]", i);
            book_lat_shards_[i].print_report(out, label);
        }
    }
    fprintf(out, "==========================================\n\n");
}

} // namespace itch
