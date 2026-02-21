#include "level2_book.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/protocol/itch_messages.hpp"
#include <algorithm>
#include <cstring>

namespace itch {

Level2Book::Level2Book(LocateCode locate, OrderPool& pool, OrderMap& map) noexcept
    : locate_(locate), pool_(pool), map_(map) {
    std::memset(bid_levels_, 0, sizeof(bid_levels_));
    std::memset(ask_levels_, 0, sizeof(ask_levels_));
    // Initialise head/tail indices to kInvalidIdx
    for (auto& lvl : bid_levels_) {
        lvl.head_order_idx = kInvalidIdx;
        lvl.tail_order_idx = kInvalidIdx;
    }
    for (auto& lvl : ask_levels_) {
        lvl.head_order_idx = kInvalidIdx;
        lvl.tail_order_idx = kInvalidIdx;
    }
}

// -----------------------------------------------------------------
// clear() — session reset (no order walking; ShardWorker bulk-resets pool/map)
// -----------------------------------------------------------------
void Level2Book::clear() noexcept {
    std::memset(bid_levels_, 0, sizeof(bid_levels_));
    std::memset(ask_levels_, 0, sizeof(ask_levels_));
    for (auto& lvl : bid_levels_) {
        lvl.head_order_idx = kInvalidIdx;
        lvl.tail_order_idx = kInvalidIdx;
    }
    for (auto& lvl : ask_levels_) {
        lvl.head_order_idx = kInvalidIdx;
        lvl.tail_order_idx = kInvalidIdx;
    }
    bid_count_ = 0;
    ask_count_ = 0;
    status_    = TradingStatus::Halted;
}

// -----------------------------------------------------------------
// Price level array helpers
// -----------------------------------------------------------------

// Linear scan for price. Returns index, or count if not found.
// Linear scan beats binary search for small N (≤256) due to prefetch.
uint16_t Level2Book::find_level(const PriceLevel* levels, uint16_t count,
                                 Price price) const noexcept {
    for (uint16_t i = 0; i < count; ++i) {
        if (levels[i].price == price)
            return i;
    }
    return count; // not found
}

// Insert a new price level at the correct sorted position.
// Bids: descending (best=highest at index 0).
// Asks: ascending  (best=lowest  at index 0).
// Returns the new level's index, or MAX_PRICE_LEVELS on overflow.
// On overflow: increments the shard's price_level_drops counter (relaxed).
// No logging on the hot path — MetricsReporter surfaces the counter periodically.
uint16_t Level2Book::insert_level(PriceLevel* levels, uint16_t& count,
                                   Price price, Side side) noexcept {
    if (ITCH_UNLIKELY(count >= MAX_PRICE_LEVELS)) {
        // Increment per-shard counter; MetricsReporter logs if > 0.
        if (drop_ctr_)
            drop_ctr_->fetch_add(1, std::memory_order_relaxed);
        return MAX_PRICE_LEVELS; // sentinel: no room
    }

    // Find insertion position (sorted)
    uint16_t pos = 0;
    if (side == Side::Buy) {
        while (pos < count && levels[pos].price > price)
            ++pos;
    } else {
        while (pos < count && levels[pos].price < price)
            ++pos;
    }

    // Shift everything at pos onwards one slot right
    if (pos < count) {
        std::memmove(&levels[pos + 1], &levels[pos],
                     (count - pos) * sizeof(PriceLevel));
    }

    // Initialise the new level
    PriceLevel& lvl = levels[pos];
    std::memset(&lvl, 0, sizeof(PriceLevel));
    lvl.price          = price;
    lvl.head_order_idx = kInvalidIdx;
    lvl.tail_order_idx = kInvalidIdx;

    ++count;
    return pos;
}

void Level2Book::remove_level(PriceLevel* levels, uint16_t& count, uint16_t idx) noexcept {
    if (idx + 1 < count) {
        std::memmove(&levels[idx], &levels[idx + 1],
                     (count - idx - 1) * sizeof(PriceLevel));
    }
    std::memset(&levels[count - 1], 0, sizeof(PriceLevel));
    levels[count - 1].head_order_idx = kInvalidIdx;
    levels[count - 1].tail_order_idx = kInvalidIdx;
    --count;
}

// -----------------------------------------------------------------
// Doubly-linked list within a PriceLevel
// -----------------------------------------------------------------

void Level2Book::link_order_tail(PriceLevel& lvl, Order* o, uint32_t idx) noexcept {
    o->prev_idx = lvl.tail_order_idx;
    o->next_idx = kInvalidIdx;

    if (lvl.tail_order_idx != kInvalidIdx)
        pool_.at(lvl.tail_order_idx)->next_idx = idx;
    else
        lvl.head_order_idx = idx; // first order in level

    lvl.tail_order_idx = idx;
    lvl.total_qty      += o->qty;
    ++lvl.order_count;
}

void Level2Book::unlink_order(PriceLevel& lvl, Order* o, uint32_t idx) noexcept {
    if (o->prev_idx != kInvalidIdx)
        pool_.at(o->prev_idx)->next_idx = o->next_idx;
    else
        lvl.head_order_idx = o->next_idx;

    if (o->next_idx != kInvalidIdx)
        pool_.at(o->next_idx)->prev_idx = o->prev_idx;
    else
        lvl.tail_order_idx = o->prev_idx;

    lvl.total_qty  -= o->qty;
    --lvl.order_count;
}

// -----------------------------------------------------------------
// on_add_order  'A' / 'F'
// -----------------------------------------------------------------
void Level2Book::on_add_order(const ItchMsg& msg) noexcept {
    const auto& a = msg.add_order;

    Order* o = pool_.alloc();
    if (ITCH_UNLIKELY(!o)) {
        ITCH_LOG_ERROR("Level2Book[%u]: OrderPool exhausted on add %lu", locate_, a.ref);
        return;
    }

    PriceLevel* levels = (a.side == Side::Buy) ? bid_levels_ : ask_levels_;
    uint16_t&   count  = (a.side == Side::Buy) ? bid_count_  : ask_count_;

    uint16_t lvl_idx = find_level(levels, count, a.price);
    if (lvl_idx == count) {
        // New price level needed
        lvl_idx = insert_level(levels, count, a.price, a.side);
        if (ITCH_UNLIKELY(lvl_idx == MAX_PRICE_LEVELS)) {
            pool_.free(pool_.index_of(o));
            return;
        }
    }

    uint32_t pool_idx = pool_.index_of(o);
    o->id        = a.ref;
    o->price     = a.price;
    o->qty       = a.shares;
    o->side      = a.side;
    o->level_idx = lvl_idx;

    link_order_tail(levels[lvl_idx], o, pool_idx);
    map_.insert(a.ref, pool_idx);
}

// -----------------------------------------------------------------
// Reduce qty helper (shared by on_order_executed / _price / cancel)
// -----------------------------------------------------------------
void Level2Book::reduce_qty(OrderId ref, Qty by_qty) noexcept {
    uint32_t pool_idx = map_.find(ref);
    if (ITCH_UNLIKELY(pool_idx == kMapEmpty)) {
        ITCH_LOG_WARN("Level2Book[%u]: reduce_qty on unknown order %lu", locate_, ref);
        return;
    }

    Order* o = pool_.at(pool_idx);
    PriceLevel* levels = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
    uint16_t&   count  = (o->side == Side::Buy) ? bid_count_  : ask_count_;

    PriceLevel& lvl = levels[o->level_idx];
    lvl.total_qty  -= by_qty;
    o->qty         -= by_qty;

    if (o->qty == 0) {
        // Fully consumed — remove from level and pool
        unlink_order(lvl, o, pool_idx);
        if (lvl.order_count == 0)
            remove_level(levels, count, o->level_idx);
        map_.erase(ref);
        pool_.free(pool_idx);
    }
    // (partial: order stays in level with reduced qty)
}

// -----------------------------------------------------------------
// on_order_executed  'E'
// -----------------------------------------------------------------
void Level2Book::on_order_executed(const ItchMsg& msg) noexcept {
    reduce_qty(msg.order_executed.ref, msg.order_executed.executed_shares);
}

// -----------------------------------------------------------------
// on_order_executed_price  'C'
// -----------------------------------------------------------------
void Level2Book::on_order_executed_price(const ItchMsg& msg) noexcept {
    reduce_qty(msg.order_executed_price.ref, msg.order_executed_price.executed_shares);
}

// -----------------------------------------------------------------
// on_order_cancel  'X'
// -----------------------------------------------------------------
void Level2Book::on_order_cancel(const ItchMsg& msg) noexcept {
    reduce_qty(msg.order_cancel.ref, msg.order_cancel.cancelled_shares);
}

// -----------------------------------------------------------------
// on_order_delete  'D'
// -----------------------------------------------------------------
void Level2Book::on_order_delete(const ItchMsg& msg) noexcept {
    uint32_t pool_idx = map_.find(msg.order_delete.ref);
    if (ITCH_UNLIKELY(pool_idx == kMapEmpty)) {
        ITCH_LOG_WARN("Level2Book[%u]: delete unknown order %lu", locate_, msg.order_delete.ref);
        return;
    }

    Order* o = pool_.at(pool_idx);
    PriceLevel* levels = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
    uint16_t&   count  = (o->side == Side::Buy) ? bid_count_  : ask_count_;
    PriceLevel& lvl    = levels[o->level_idx];

    unlink_order(lvl, o, pool_idx);
    if (lvl.order_count == 0)
        remove_level(levels, count, o->level_idx);

    map_.erase(msg.order_delete.ref);
    pool_.free(pool_idx);
}

// -----------------------------------------------------------------
// on_order_replace  'U'
//
// NASDAQ ITCH 5.0 spec §4.6.2 — time priority rule:
//
//   Case A — same price AND new_shares <= old_shares:
//     In-place quantity update.  The order KEEPS its position in the
//     FIFO queue (time priority preserved).  Only qty and the order
//     reference number change; the Order struct is not moved.
//     Rationale: reducing qty is a passive action that does not warrant
//     losing queue position; NASDAQ treats it as a simple amendment.
//
//   Case B — price changed OR new_shares > old_shares:
//     Full replacement.  The old order is deleted from its price level
//     and a brand-new order is appended to the TAIL of the target level
//     (new time priority).  Increasing qty is equivalent to cancelling
//     the original order and submitting a new, larger one — the exchange
//     sends the firm to the back of the queue.
//
// BBO is updated by ShardWorker immediately after this call returns.
// -----------------------------------------------------------------
void Level2Book::on_order_replace(const ItchMsg& msg) noexcept {
    const auto& r = msg.order_replace;

    uint32_t old_idx = map_.find(r.orig_ref);
    if (ITCH_UNLIKELY(old_idx == kMapEmpty)) {
        ITCH_LOG_WARN("Level2Book[%u]: replace unknown orig_ref %lu", locate_, r.orig_ref);
        return;
    }

    Order*      old_o     = pool_.at(old_idx);
    const Side  side      = old_o->side;
    const Price old_price = old_o->price;
    const Qty   old_qty   = old_o->qty;

    PriceLevel* levels = (side == Side::Buy) ? bid_levels_ : ask_levels_;
    uint16_t&   count  = (side == Side::Buy) ? bid_count_  : ask_count_;

    if (r.new_price == old_price && r.new_shares <= old_qty) {
        // ── Case A: in-place (time priority preserved) ──────────────────────
        // The order stays at its current position in the FIFO list.
        // Only qty, the level's aggregate, and the map key change.
        PriceLevel& lvl = levels[old_o->level_idx];
        lvl.total_qty  -= old_qty;
        lvl.total_qty  += r.new_shares;
        old_o->qty      = r.new_shares;
        old_o->id       = r.new_ref;

        // Re-key: remove orig_ref, add new_ref pointing to same pool slot.
        map_.erase(r.orig_ref);
        map_.insert(r.new_ref, old_idx);

        if (inplace_ctr_)
            inplace_ctr_->fetch_add(1, std::memory_order_relaxed);
    } else {
        // ── Case B: full replacement (new time priority) ─────────────────────
        // Unlink old order from its price level, free the pool slot, then
        // allocate a fresh order and append it to the tail of new_price's level.
        PriceLevel& old_lvl = levels[old_o->level_idx];
        unlink_order(old_lvl, old_o, old_idx);
        if (old_lvl.order_count == 0)
            remove_level(levels, count, old_o->level_idx);

        map_.erase(r.orig_ref);
        pool_.free(old_idx);

        Order* new_o = pool_.alloc();
        if (ITCH_UNLIKELY(!new_o)) {
            ITCH_LOG_ERROR("Level2Book[%u]: pool exhausted on replace new_ref=%lu",
                           locate_, r.new_ref);
            return;
        }
        uint32_t new_idx = pool_.index_of(new_o);

        uint16_t lvl_idx = find_level(levels, count, r.new_price);
        if (lvl_idx == count)
            lvl_idx = insert_level(levels, count, r.new_price, side);
        if (ITCH_UNLIKELY(lvl_idx == MAX_PRICE_LEVELS)) {
            pool_.free(new_idx);
            return;
        }

        new_o->id        = r.new_ref;
        new_o->price     = r.new_price;
        new_o->qty       = r.new_shares;
        new_o->side      = side;
        new_o->level_idx = lvl_idx;

        link_order_tail(levels[lvl_idx], new_o, new_idx);
        map_.insert(r.new_ref, new_idx);
    }
}

// -----------------------------------------------------------------
// on_trading_action  'H'
// -----------------------------------------------------------------
void Level2Book::on_trading_action(const ItchMsg& msg) noexcept {
    status_ = static_cast<TradingStatus>(msg.trading_action.trading_state);
    ITCH_LOG_INFO("Level2Book[%u]: trading state → '%c'", locate_,
                  msg.trading_action.trading_state);
}

// -----------------------------------------------------------------
// BBO
// -----------------------------------------------------------------
BboSnapshot Level2Book::get_bbo() const noexcept {
    BboSnapshot bbo{};
    bbo.locate = locate_;
    if (bid_count_ > 0) {
        bbo.bid_price = bid_levels_[0].price;
        bbo.bid_qty   = bid_levels_[0].total_qty;
    }
    if (ask_count_ > 0) {
        bbo.ask_price = ask_levels_[0].price;
        bbo.ask_qty   = ask_levels_[0].total_qty;
    }
    return bbo;
}

// -----------------------------------------------------------------
// Snapshot serialisation (simple binary format)
// -----------------------------------------------------------------
std::size_t Level2Book::serialize_snapshot(uint8_t* buf, std::size_t buf_size) const noexcept {
    // Format: [locate:2][bid_count:2][ask_count:2][bid_levels...][ask_levels...]
    // counts are uint16_t so the format can represent MAX_PRICE_LEVELS=256 exactly.
    static constexpr std::size_t kHeaderSize = 6; // 2+2+2
    std::size_t needed = kHeaderSize +
        bid_count_ * sizeof(PriceLevel) +
        ask_count_ * sizeof(PriceLevel);
    if (buf_size < needed)
        return 0;

    uint8_t* p = buf;
    std::memcpy(p, &locate_,    2); p += 2;
    std::memcpy(p, &bid_count_, 2); p += 2;
    std::memcpy(p, &ask_count_, 2); p += 2;
    std::memcpy(p, bid_levels_, bid_count_ * sizeof(PriceLevel)); p += bid_count_ * sizeof(PriceLevel);
    std::memcpy(p, ask_levels_, ask_count_ * sizeof(PriceLevel));
    return needed;
}

bool Level2Book::restore_snapshot(const uint8_t* buf, std::size_t len) noexcept {
    static constexpr std::size_t kHeaderSize = 6;
    if (len < kHeaderSize)
        return false;

    const uint8_t* p = buf;
    LocateCode loc;
    std::memcpy(&loc, p, 2); p += 2;
    uint16_t bc, ac;
    std::memcpy(&bc,  p, 2); p += 2;
    std::memcpy(&ac,  p, 2); p += 2;

    if (bc > MAX_PRICE_LEVELS || ac > MAX_PRICE_LEVELS)
        return false;

    std::size_t needed = kHeaderSize + bc * sizeof(PriceLevel) + ac * sizeof(PriceLevel);
    if (len < needed)
        return false;

    locate_    = loc;
    bid_count_ = bc;
    ask_count_ = ac;
    std::memcpy(bid_levels_, p, bc * sizeof(PriceLevel)); p += bc * sizeof(PriceLevel);
    std::memcpy(ask_levels_, p, ac * sizeof(PriceLevel));
    return true;
}

} // namespace itch
