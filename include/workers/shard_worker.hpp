#pragma once
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "../monitoring/latency_histogram.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../orderbook/bbo_publisher.hpp"
#include "../orderbook/level2_book.hpp"
#include "../orderbook/order_book_pool.hpp"
#include "../routing/shard_router.hpp"
#include "../routing/symbol_directory.hpp"
#include "../session/session_manager.hpp"
#include "../system/cpu_affinity.hpp"
#include <atomic>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// ShardWorker — processes all ITCH messages for one shard.
//
// Owns Level2Book instances for all locate codes in this shard
// (i.e., locate % NUM_SHARDS == shard_id).
// No locks: single writer to each book.
// -----------------------------------------------------------------
class ShardWorker {
  public:
    ShardWorker(ShardId                shard_id,
                ShardQueue&            queue,
                BboPublisher&          bbo_pub,
                LatencyHistogram&      book_lat,
                const SymbolDirectory& sym_dir,
                OrderPool&             pool,
                OrderMap&              map,
                OrderBookPool&         book_pool,
                const SessionManager&  session_mgr,
                FeedStats&             stats,
                const ThreadConfig&    cfg) noexcept;

    ~ShardWorker() { stop(); }

    void start();
    void stop();

    ShardId shard_id() const noexcept { return shard_id_; }

  private:
    ShardId                shard_id_;
    ShardQueue&            queue_;
    BboPublisher&          bbo_pub_;
    LatencyHistogram&      book_lat_;
    const SymbolDirectory& sym_dir_;
    OrderPool&             pool_;
    OrderMap&              map_;
    OrderBookPool&         book_pool_;
    const SessionManager&  session_mgr_;
    FeedStats&             stats_;
    ThreadConfig           cfg_;

    // Tracks which slab slots have been activated by a StockDirectory message.
    // Index = loc / NUM_SHARDS (same mapping as OrderBookPool::book()).
    bool book_active_[OrderBookPool::BOOKS_PER_SHARD]{};

    std::atomic<bool> running_{false};
    std::thread       thread_;

    void run() noexcept;
    void dispatch(const ItchMsg& msg) noexcept;
    // Mark the pre-allocated book for `loc` as active (called on StockDirectory).
    Level2Book* activate_book(LocateCode loc, const char* stock8) noexcept;
    // Called on InternalSessionReset: clear all active books in-place.
    // pool_ and map_ are bulk-reset immediately after.
    void clear_all_books() noexcept;
};

} // namespace itch
