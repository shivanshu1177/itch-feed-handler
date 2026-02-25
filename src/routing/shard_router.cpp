#include "shard_router.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/monitoring/metrics_reporter.hpp"

namespace itch {

ShardRouter::ShardRouter(const SymbolDirectory& dir,
                         FeedStats&             stats,
                         std::size_t            pressure_threshold) noexcept
    : dir_(dir), stats_(stats), pressure_threshold_(pressure_threshold) {
    for (auto& q : queues_) {
        q = std::make_unique<ShardQueue>();
    }
    ITCH_LOG_INFO("ShardRouter created: %d shards, queue depth %d, pressure threshold %zu",
                  NUM_SHARDS, SHARD_QUEUE_DEPTH, pressure_threshold_);
}

bool ShardRouter::is_admin_message(MessageType t) noexcept {
    switch (t) {
    case MessageType::SystemEvent:
    case MessageType::StockDirectory:
    case MessageType::StockTradingAction:
    case MessageType::RegSHO:
    case MessageType::MWCBDecline:
    case MessageType::MWCBStatus:
    case MessageType::OperationalHalt:
        return true;
    default:
        return false;
    }
}

RouteResult ShardRouter::route(const ItchMsg& msg) noexcept {
    if (is_admin_message(msg.type)) {
        return broadcast(msg);
    }

    ShardId shard = dir_.shard_for_locate(msg.locate);
    if (ITCH_LIKELY(queues_[shard]->try_push(msg))) {
        return RouteResult::Ok;
    }

    // Queue full — count the drop; no synchronous log (hot path).
    stats_.shard_queue_drops[shard].fetch_add(1, std::memory_order_relaxed);
    return RouteResult::QueueFull;
}

RouteResult ShardRouter::broadcast(const ItchMsg& msg) noexcept {
    RouteResult result = RouteResult::Ok;
    for (ShardId s = 0; s < NUM_SHARDS; ++s) {
        if (ITCH_UNLIKELY(!queues_[s]->try_push(msg))) {
            stats_.shard_queue_drops[s].fetch_add(1, std::memory_order_relaxed);
            result = RouteResult::QueueFull;
        }
    }
    return result;
}

} // namespace itch
