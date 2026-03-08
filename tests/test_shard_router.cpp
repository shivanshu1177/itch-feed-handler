#include <gtest/gtest.h>
#include "monitoring/metrics_reporter.hpp"
#include "routing/shard_router.hpp"
#include "routing/symbol_directory.hpp"

using namespace itch;

// Helper: default FeedStats + 80% pressure threshold for tests.
static FeedStats g_stats;
static constexpr std::size_t kThreshold =
    static_cast<std::size_t>(SHARD_QUEUE_DEPTH * 0.8);

TEST(ShardRouter, LocateToShardMapping) {
    SymbolDirectory dir;
    ShardRouter router(dir, g_stats, kThreshold);

    // locate % NUM_SHARDS determines the shard
    for (LocateCode loc = 1; loc < 64; ++loc) {
        ShardId expected = static_cast<ShardId>(loc % NUM_SHARDS);
        EXPECT_EQ(dir.shard_for_locate(loc), expected);
    }
}

TEST(ShardRouter, RouteToCorrectShard) {
    SymbolDirectory dir;
    ShardRouter router(dir, g_stats, kThreshold);

    ItchMsg msg{};
    msg.type = MessageType::AddOrder;
    msg.locate = 5; // 5 % 8 = 5 → shard 5

    EXPECT_EQ(router.route(msg), RouteResult::Ok);
    EXPECT_EQ(router.queue_depth(5), 1u);
    for (ShardId s = 0; s < NUM_SHARDS; ++s) {
        if (s != 5)
            EXPECT_EQ(router.queue_depth(s), 0u);
    }
}

TEST(ShardRouter, AdminMessageBroadcast) {
    SymbolDirectory dir;
    ShardRouter router(dir, g_stats, kThreshold);

    ItchMsg msg{};
    msg.type   = MessageType::SystemEvent;
    msg.locate = 1;

    router.route(msg);
    // All shards should have received the admin message
    for (ShardId s = 0; s < NUM_SHARDS; ++s) {
        EXPECT_EQ(router.queue_depth(s), 1u) << "Shard " << (int)s << " missing admin msg";
    }
}
