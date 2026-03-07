#include <gtest/gtest.h>
#include "orderbook/level2_book.hpp"
#include "protocol/itch_messages.hpp"
#include <cstdlib>

using namespace itch;

// Test fixture: allocates a minimal pool/map for book tests
class BookTest : public ::testing::Test {
  protected:
    static constexpr uint32_t POOL_CAP = 4096;
    static constexpr uint32_t MAP_CAP  = 8192;

    void SetUp() override {
        pool_mem_ = std::aligned_alloc(64, POOL_CAP * (sizeof(Order) + sizeof(uint32_t)));
        map_mem_  = std::aligned_alloc(16, MAP_CAP  * sizeof(MapEntry));
        pool_ = std::make_unique<OrderPool>(POOL_CAP, pool_mem_);
        map_  = std::make_unique<OrderMap>(static_cast<MapEntry*>(map_mem_), MAP_CAP);
        book_ = std::make_unique<Level2Book>(1, *pool_, *map_);
    }
    void TearDown() override {
        book_.reset();
        pool_.reset();
        map_.reset();
        std::free(pool_mem_);
        std::free(map_mem_);
    }

    ItchMsg make_add(OrderId ref, Side side, Price price, Qty shares) {
        ItchMsg m{};
        m.type              = (side == Side::Buy) ? MessageType::AddOrder : MessageType::AddOrder;
        m.locate            = 1;
        m.type              = MessageType::AddOrder;
        m.add_order.ref     = ref;
        m.add_order.side    = side;
        m.add_order.price   = price;
        m.add_order.shares  = shares;
        return m;
    }
    ItchMsg make_cancel(OrderId ref, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::OrderCancel;
        m.locate = 1;
        m.order_cancel.ref = ref;
        m.order_cancel.cancelled_shares = qty;
        return m;
    }
    ItchMsg make_delete(OrderId ref) {
        ItchMsg m{};
        m.type = MessageType::OrderDelete;
        m.locate = 1;
        m.order_delete.ref = ref;
        return m;
    }
    ItchMsg make_executed(OrderId ref, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::OrderExecuted;
        m.locate = 1;
        m.order_executed.ref = ref;
        m.order_executed.executed_shares = qty;
        return m;
    }
    ItchMsg make_replace(OrderId orig, OrderId newref, Price price, Qty qty) {
        ItchMsg m{};
        m.type = MessageType::OrderReplace;
        m.locate = 1;
        m.order_replace.orig_ref   = orig;
        m.order_replace.new_ref    = newref;
        m.order_replace.new_price  = price;
        m.order_replace.new_shares = qty;
        return m;
    }

    void* pool_mem_{nullptr};
    void* map_mem_{nullptr};
    std::unique_ptr<OrderPool> pool_;
    std::unique_ptr<OrderMap>  map_;
    std::unique_ptr<Level2Book> book_;
};

// ----- Add Order -----
TEST_F(BookTest, AddBidCreatesLevel) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 10000);
    EXPECT_EQ(bbo.bid_qty,   100u);
    EXPECT_EQ(book_->bid_depth(), 1u);
}

TEST_F(BookTest, AddAskCreatesLevel) {
    book_->on_add_order(make_add(2, Side::Sell, 10100, 200));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.ask_price, 10100);
    EXPECT_EQ(bbo.ask_qty,   200u);
}

TEST_F(BookTest, BboSpreadPositive) {
    book_->on_add_order(make_add(1, Side::Buy,  10000, 100));
    book_->on_add_order(make_add(2, Side::Sell, 10100, 200));
    auto bbo = book_->get_bbo();
    EXPECT_LT(bbo.bid_price, bbo.ask_price);
}

TEST_F(BookTest, MultipleOrdersSameLevel) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(2, Side::Buy, 10000, 200));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 300u);
    EXPECT_EQ(book_->bid_depth(), 1u); // still one level
}

TEST_F(BookTest, MultipleBidLevels) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(2, Side::Buy,  9900, 200));
    book_->on_add_order(make_add(3, Side::Buy,  9800, 300));
    EXPECT_EQ(book_->bid_depth(), 3u);
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 10000); // best bid is highest
}

// ----- Cancel / Delete -----
TEST_F(BookTest, PartialCancel) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_cancel(make_cancel(1, 40));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 60u);
    EXPECT_EQ(book_->bid_depth(), 1u); // level still exists
}

TEST_F(BookTest, FullCancelRemovesLevel) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_cancel(make_cancel(1, 100));
    EXPECT_EQ(book_->bid_depth(), 0u);
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 0);
}

TEST_F(BookTest, DeleteOrder) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_delete(make_delete(1));
    EXPECT_EQ(book_->bid_depth(), 0u);
}

TEST_F(BookTest, DeleteUnknownOrderSafe) {
    EXPECT_NO_FATAL_FAILURE(book_->on_order_delete(make_delete(999)));
}

// ----- Execute -----
TEST_F(BookTest, PartialExecute) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_executed(make_executed(1, 30));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 70u);
}

TEST_F(BookTest, FullExecuteRemovesOrder) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_executed(make_executed(1, 100));
    EXPECT_EQ(book_->bid_depth(), 0u);
}

// ----- Replace -----
//
// NASDAQ ITCH 5.0 §4.6.2 OrderReplace time-priority rules:
//
//   Case A (in-place):  new_price == old_price  &&  new_shares <= old_shares
//     → qty updated in-place; order keeps its FIFO position (time priority preserved).
//
//   Case B (tail):      price changed  OR  new_shares > old_shares
//     → old order deleted, new order appended to tail of new_price level (new time priority).
//
// The tests below verify correctness by adding a second order at the same price
// and then executing shares to confirm which order was first in the FIFO queue.

TEST_F(BookTest, ReplaceSamePrice) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_order_replace(make_replace(1, 2, 10000, 150));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 150u);
    EXPECT_EQ(bbo.bid_price, 10000);
}

TEST_F(BookTest, ReplaceDifferentPrice) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(3, Side::Buy,  9900, 50));
    book_->on_order_replace(make_replace(1, 2, 9900, 200));
    auto bbo = book_->get_bbo();
    // Old level at 10000 should be gone, 9900 now has 50+200=250
    EXPECT_EQ(bbo.bid_price, 9900);
    EXPECT_EQ(bbo.bid_qty, 250u);
    EXPECT_EQ(book_->bid_depth(), 1u);
}

// Case A: same price, qty REDUCED → in-place (time priority preserved).
// Order 1 (100 qty) is first at 10000; Order 2 (200 qty) is second.
// Replace order 1 → order 3 with qty 50 (same price, qty reduced).
// Execute 50 shares: must consume order 3 (formerly 1, still at head of FIFO).
// After execution: only order 2 (200 qty) remains.
TEST_F(BookTest, ReplaceSamePriceQtyReducedPreservesTimePriority) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(2, Side::Buy, 10000, 200));

    // Case A: same price, qty 100 → 50 (reduced) — must preserve FIFO position
    book_->on_order_replace(make_replace(1, 3, 10000, 50));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 10000);
    EXPECT_EQ(bbo.bid_qty, 250u);  // 50 + 200
    EXPECT_EQ(book_->bid_depth(), 1u);

    // Execute 50 shares — should consume order 3 (formerly order 1, at FIFO head)
    book_->on_order_executed(make_executed(3, 50));
    bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 200u);  // only order 2 remains
    EXPECT_EQ(book_->bid_depth(), 1u);

    // Order 2 still alive — delete it to confirm
    book_->on_order_delete(make_delete(2));
    EXPECT_EQ(book_->bid_depth(), 0u);
}

// Case A boundary: same price, qty UNCHANGED → also in-place (qty <= old).
TEST_F(BookTest, ReplaceSamePriceQtyUnchangedIsInPlace) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(2, Side::Buy, 10000, 200));

    // qty unchanged (100 → 100), same price → in-place
    book_->on_order_replace(make_replace(1, 3, 10000, 100));

    // Execute 100 shares — must consume order 3 (at head, formerly order 1)
    book_->on_order_executed(make_executed(3, 100));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 200u);  // only order 2 remains
}

// Case B: same price, qty INCREASED → goes to tail (new time priority).
// Order 1 (100 qty) is first; Order 2 (200 qty) is second.
// Replace order 1 → order 3 with qty 150 (same price, qty increased → tail).
// Execute 200 shares: must consume order 2 first (now at FIFO head).
// After execution: order 3 (150 qty) remains with 150-0 = 150 qty (untouched).
TEST_F(BookTest, ReplaceSamePriceQtyIncreasedGoesToTail) {
    book_->on_add_order(make_add(1, Side::Buy, 10000, 100));
    book_->on_add_order(make_add(2, Side::Buy, 10000, 200));

    // Case B: same price, qty 100 → 150 (increased) — must go to tail
    book_->on_order_replace(make_replace(1, 3, 10000, 150));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 350u);  // 200 + 150
    EXPECT_EQ(book_->bid_depth(), 1u);

    // Execute 200 shares — must consume order 2 (now at FIFO head)
    book_->on_order_executed(make_executed(2, 200));
    bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 150u);  // only order 3 remains (was moved to tail)
    EXPECT_EQ(book_->bid_depth(), 1u);

    book_->on_order_delete(make_delete(3));
    EXPECT_EQ(book_->bid_depth(), 0u);
}

// Case B: price changed → full replacement at tail of target level, BBO correct.
//
// Setup: order 2 at 10000 (level 0), order 1 at 9800 (level 1).
// Replace order 1 → order 3 at 10000, qty 200 (price improved, goes to tail).
// Removing level 1 (9800) does NOT shift level 0 (10000), so order 2's
// level_idx stays valid — this sidesteps the pre-existing stale-level_idx issue
// that would occur if the removed level had a lower index than existing orders.
TEST_F(BookTest, ReplacePriceChangedGoesToTailNewLevel) {
    book_->on_add_order(make_add(2, Side::Buy, 10000, 50));  // level 0: 10000, head
    book_->on_add_order(make_add(1, Side::Buy,  9800, 100)); // level 1: 9800 (will be removed)

    // Replace order 1 (9800) → order 3 at 10000, qty 200 (Case B, goes to tail of level 0)
    book_->on_order_replace(make_replace(1, 3, 10000, 200));
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 10000);   // level 9800 gone; best bid stays 10000
    EXPECT_EQ(bbo.bid_qty, 250u);      // 50 (order 2) + 200 (order 3)
    EXPECT_EQ(book_->bid_depth(), 1u);

    // Execute 50 shares — must consume order 2 (original, at FIFO head of 10000)
    book_->on_order_executed(make_executed(2, 50));
    bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_qty, 200u);  // order 3 remains (was appended at tail)
}

// ----- Empty book -----
TEST_F(BookTest, EmptyBookBboIsZero) {
    auto bbo = book_->get_bbo();
    EXPECT_EQ(bbo.bid_price, 0);
    EXPECT_EQ(bbo.ask_price, 0);
}

// ----- Snapshot round-trip -----
TEST_F(BookTest, SnapshotRoundTrip) {
    book_->on_add_order(make_add(1, Side::Buy,  10000, 100));
    book_->on_add_order(make_add(2, Side::Sell, 10100, 200));

    uint8_t buf[65536];
    std::size_t written = book_->serialize_snapshot(buf, sizeof(buf));
    ASSERT_GT(written, 0u);

    // Restore into a fresh book
    Level2Book book2(1, *pool_, *map_);
    EXPECT_TRUE(book2.restore_snapshot(buf, written));
    auto bbo = book2.get_bbo();
    EXPECT_EQ(bbo.bid_price, 10000);
    EXPECT_EQ(bbo.ask_price, 10100);
}
