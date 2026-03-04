#include <gtest/gtest.h>
#include "orderbook/order_pool.hpp"
#include <cstdlib>
#include <vector>

using namespace itch;

class OrderPoolTest : public ::testing::Test {
  protected:
    static constexpr uint32_t CAP = 1024;
    void SetUp() override {
        mem_ = std::aligned_alloc(64, CAP * (sizeof(Order) + sizeof(uint32_t)));
        pool_ = std::make_unique<OrderPool>(CAP, mem_);
    }
    void TearDown() override {
        pool_.reset();
        std::free(mem_);
    }
    void* mem_{nullptr};
    std::unique_ptr<OrderPool> pool_;
};

TEST_F(OrderPoolTest, AllocFree) {
    Order* o = pool_->alloc();
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(pool_->allocated(), 1u);
    pool_->free(pool_->index_of(o));
    EXPECT_EQ(pool_->allocated(), 0u);
}

TEST_F(OrderPoolTest, AllocToCapacity) {
    std::vector<Order*> ptrs;
    for (uint32_t i = 0; i < CAP; ++i) {
        Order* o = pool_->alloc();
        ASSERT_NE(o, nullptr) << "Alloc failed at i=" << i;
        ptrs.push_back(o);
    }
    EXPECT_EQ(pool_->allocated(), CAP);
    EXPECT_EQ(pool_->alloc(), nullptr); // exhausted
    // Free all
    for (Order* o : ptrs)
        pool_->free(pool_->index_of(o));
    EXPECT_EQ(pool_->allocated(), 0u);
}

TEST_F(OrderPoolTest, ReuseAfterFree) {
    Order* o1 = pool_->alloc();
    uint32_t idx1 = pool_->index_of(o1);
    pool_->free(idx1);
    Order* o2 = pool_->alloc();
    EXPECT_EQ(pool_->index_of(o2), idx1); // LIFO reuse
}
