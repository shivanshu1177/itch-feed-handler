#pragma once
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "latency_histogram.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// MetricsReporter — background thread that periodically prints
// latency histograms and throughput counters to stderr.
//
// Not pinned to any specific core (low-priority background work).
// Reads atomic counters with relaxed ordering — statistical only.
// -----------------------------------------------------------------
struct FeedStats {
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_dropped{0};   // ring full
    std::atomic<uint64_t> messages_parsed{0};
    std::atomic<uint64_t> sequence_gaps{0};
    std::atomic<uint64_t> sequence_dups{0};
    // Per-shard queue-full drop counters (incremented by ShardRouter, ParserThread only).
    // Single-writer per counter → share one cache line intentionally.
    alignas(64) std::atomic<uint64_t> shard_queue_drops[NUM_SHARDS]{};
    // Per-shard price-level overflow counters (incremented by ShardWorker, one writer each).
    // Each shard is a different writer → separate cache line to avoid false sharing.
    alignas(64) std::atomic<uint64_t> price_level_drops[NUM_SHARDS]{};
    // Recovery counters — written by RecoveryThread only.
    alignas(64) std::atomic<uint64_t> recovered_packets{0};
    std::atomic<uint64_t>             recovery_drops{0};
    // Per-shard in-place replace counters (incremented by ShardWorker[s], one writer each).
    // "In-place" = same price + qty-reduced: time priority preserved per NASDAQ ITCH 5.0 spec.
    alignas(64) std::atomic<uint64_t> order_replaces_inplace[NUM_SHARDS]{};
    // A/B feed merger counters — written by FeedMerger only.
    alignas(64) std::atomic<uint64_t> secondary_packets{0};  // total from secondary feed
    std::atomic<uint64_t>             dedup_drops{0};        // duplicates suppressed
    std::atomic<uint64_t>             feed_failover_count{0};// times primary declared lost
    std::atomic<uint64_t>             merged_drops{0};       // merged queue full drops
    // Per-message-type counters (indexed by MessageType char value)
    alignas(64) std::atomic<uint64_t> msg_type_count[256]{};
};

class MetricsReporter {
  public:
    // report_interval_sec: how often to print (from config)
    MetricsReporter(uint32_t report_interval_sec,
                    const FeedStats& stats,
                    LatencyHistogram* parse_lat,
                    LatencyHistogram* book_lat_shards, // array[NUM_SHARDS]
                    uint32_t num_shards);

    ~MetricsReporter();

    void start();
    void stop();

  private:
    uint32_t interval_sec_;
    const FeedStats& stats_;
    LatencyHistogram* parse_lat_;
    LatencyHistogram* book_lat_shards_;
    uint32_t num_shards_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    void run() noexcept;
    void print_report() noexcept;
};

} // namespace itch
