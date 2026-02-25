#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "../concurrency/spsc_queue.hpp"
#include "../protocol/itch_messages.hpp"
#include "symbol_directory.hpp"
#include <array>
#include <cstddef>
#include <memory>

namespace itch {

// Per-shard SPSC queue (ItchMsg payload)
using ShardQueue = SpscQueue<ItchMsg, SHARD_QUEUE_DEPTH>;

// Forward declaration — full definition in metrics_reporter.hpp.
// ShardRouter holds a reference to FeedStats to increment drop counters
// without including the full monitoring header here.
struct FeedStats;

// Result returned by route() and broadcast().
enum class RouteResult : uint8_t {
    Ok,        // Message delivered to queue(s)
    QueueFull, // At least one target queue was full; message dropped
};

// -----------------------------------------------------------------
// ShardRouter — routes ItchMsg to one of NUM_SHARDS shard queues.
//
// Routing key: locate_code % NUM_SHARDS
// Admin messages (StockDirectory, SystemEvent, TradingAction) are
// delivered to ALL shard queues (fan-out).
//
// On queue-full: increments FeedStats::shard_queue_drops[shard] with
// relaxed ordering — no synchronous logging on the hot path.
// -----------------------------------------------------------------
class ShardRouter {
  public:
    // pressure_threshold: queue fill level (in entries) at which
    // is_pressure_high() returns true. Typically
    //   static_cast<std::size_t>(SHARD_QUEUE_DEPTH * alert_fraction).
    ShardRouter(const SymbolDirectory& dir,
                FeedStats&             stats,
                std::size_t            pressure_threshold) noexcept;

    // Route one message. Increments per-shard drop counter on queue-full.
    // Never logs synchronously.
    RouteResult route(const ItchMsg& msg) noexcept;

    // Fan-out admin message to all shards.
    // Returns QueueFull if any shard queue was full.
    RouteResult broadcast(const ItchMsg& msg) noexcept;

    ShardQueue& queue(ShardId shard) noexcept { return *queues_[shard]; }

    // Approximate depth of a shard queue (for monitoring)
    std::size_t queue_depth(ShardId shard) const noexcept {
        return queues_[shard]->size_approx();
    }

    // Returns true if shard's queue fill ≥ pressure_threshold_.
    // Uses relaxed atomics — soft hint only, not a correctness guarantee.
    ITCH_FORCE_INLINE bool is_pressure_high(ShardId shard) const noexcept {
        return queues_[shard]->is_pressure_high(pressure_threshold_);
    }

  private:
    const SymbolDirectory& dir_;
    FeedStats&             stats_;
    std::size_t            pressure_threshold_;
    std::array<std::unique_ptr<ShardQueue>, NUM_SHARDS> queues_;

    static bool is_admin_message(MessageType t) noexcept;
};

} // namespace itch
