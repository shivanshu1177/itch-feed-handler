#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "order_map.hpp"
#include "order_pool.hpp"
#include "price_level.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>

namespace itch {

// Forward declare parsed messages
struct ItchMsg;

enum class TradingStatus : uint8_t {
    Unknown   = 0,
    Halted    = 'H',
    Paused    = 'P',
    Quotation = 'Q',
    Trading   = 'T',
};

// BBO snapshot written to SeqLock
struct BboSnapshot {
    LocateCode locate     = 0;
    uint16_t   pad        = 0;
    uint32_t   pad2       = 0;
    Price      bid_price  = 0;
    Qty        bid_qty    = 0;
    Price      ask_price  = 0;
    Qty        ask_qty    = 0;
    Timestamp  ts         = 0;
};
static_assert(std::is_trivially_copyable_v<BboSnapshot>);

// -----------------------------------------------------------------
// Level2Book — maintains a full Level 2 order book for one symbol.
//
// Price levels are stored as two sorted arrays (bid descending,
// ask ascending) of MAX_PRICE_LEVELS=256 PriceLevel structs.
// BBO is always levels[0] on each side — O(1) access.
//
// All hot-path methods (on_add_order etc.) are zero-allocation:
// they use OrderPool and OrderMap which are pre-allocated.
// -----------------------------------------------------------------
class Level2Book {
  public:
    Level2Book(LocateCode locate, OrderPool& pool, OrderMap& map) noexcept;

    // ----- Hot path (called from ShardWorker) -----
    void on_add_order(const ItchMsg& msg) noexcept;
    void on_order_executed(const ItchMsg& msg) noexcept;
    void on_order_executed_price(const ItchMsg& msg) noexcept;
    void on_order_cancel(const ItchMsg& msg) noexcept;
    void on_order_delete(const ItchMsg& msg) noexcept;
    void on_order_replace(const ItchMsg& msg) noexcept;

    // ----- Cold path (administrative) -----
    void on_trading_action(const ItchMsg& msg) noexcept;

    // ----- Session reset -----
    // Clear all price levels and reset trading status to Halted.
    // Does NOT walk orders (pool/map are bulk-reset by ShardWorker).
    void clear() noexcept;

    // ----- BBO access -----
    BboSnapshot get_bbo() const noexcept;

    // ----- Book state -----
    LocateCode   locate()         const noexcept { return locate_; }
    TradingStatus trading_status() const noexcept { return status_; }
    uint8_t      bid_depth()      const noexcept { return bid_count_; }
    uint8_t      ask_depth()      const noexcept { return ask_count_; }

    // ----- Monitoring -----

    // True if either side has reached MAX_PRICE_LEVELS (all slots in use).
    // Safe to call from any thread: bid_count_ and ask_count_ are only written
    // by the owning ShardWorker, so reads are consistent for monitoring purposes.
    ITCH_FORCE_INLINE bool is_full() const noexcept {
        return bid_count_ >= MAX_PRICE_LEVELS || ask_count_ >= MAX_PRICE_LEVELS;
    }

    // Attach a per-shard drop counter (owned by FeedStats).
    // Called by ShardWorker::activate_book() before the book goes live.
    // The pointer is never null after activation; nullptr is the pre-activation default.
    ITCH_FORCE_INLINE void set_drop_ctr(std::atomic<uint64_t>* ctr) noexcept {
        drop_ctr_ = ctr;
    }

    // Attach a per-shard in-place replace counter (owned by FeedStats).
    // Incremented on every same-price + qty-reduced OrderReplace (time priority preserved).
    ITCH_FORCE_INLINE void set_inplace_ctr(std::atomic<uint64_t>* ctr) noexcept {
        inplace_ctr_ = ctr;
    }

    // ----- Recovery -----
    // Snapshot header is 6 bytes: locate(2) + bid_count(2) + ask_count(2).
    std::size_t serialize_snapshot(uint8_t* buf, std::size_t buf_size) const noexcept;
    bool        restore_snapshot(const uint8_t* buf, std::size_t len) noexcept;

  private:
    LocateCode    locate_;
    TradingStatus status_{TradingStatus::Halted};
    OrderPool&    pool_;
    OrderMap&     map_;
    // Pointer to this shard's price_level_drops counter in FeedStats.
    // Incremented (relaxed) inside insert_level() when MAX_PRICE_LEVELS is reached.
    std::atomic<uint64_t>* drop_ctr_{nullptr};
    // Pointer to this shard's order_replaces_inplace counter in FeedStats.
    // Incremented (relaxed) on same-price + qty-reduced OrderReplace (time priority preserved).
    std::atomic<uint64_t>* inplace_ctr_{nullptr};

    alignas(64) PriceLevel bid_levels_[MAX_PRICE_LEVELS]{};
    alignas(64) PriceLevel ask_levels_[MAX_PRICE_LEVELS]{};
    // uint16_t so that count can represent the full range [0, MAX_PRICE_LEVELS].
    // uint8_t would wrap to 0 at count==255 after the 256th insertion,
    // silently destroying the book's depth state.
    uint16_t bid_count_{0};
    uint16_t ask_count_{0};

    // Internal helpers — all indices and counts are uint16_t to match count fields.
    uint16_t find_level(const PriceLevel* levels, uint16_t count,
                        Price price) const noexcept;
    // Returns new level index on success, MAX_PRICE_LEVELS on overflow.
    uint16_t insert_level(PriceLevel* levels, uint16_t& count,
                          Price price, Side side) noexcept;
    void remove_level(PriceLevel* levels, uint16_t& count, uint16_t idx) noexcept;
    void link_order_tail(PriceLevel& lvl, Order* o, uint32_t idx) noexcept;
    void unlink_order(PriceLevel& lvl, Order* o, uint32_t idx) noexcept;
    void reduce_qty(OrderId ref, Qty by_qty) noexcept;
};

} // namespace itch
