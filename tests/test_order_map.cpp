#include <gtest/gtest.h>
#include "orderbook/order_map.hpp"
#include <cstdlib>
#include <unordered_map>

using namespace itch;

class OrderMapTest : public ::testing::Test {
  protected:
    static constexpr uint32_t CAP = 1024; // small cap for tests
    void SetUp() override {
        mem_ = static_cast<MapEntry*>(
            std::aligned_alloc(16, CAP * sizeof(MapEntry)));
        map_ = std::make_unique<OrderMap>(mem_, CAP);
    }
    void TearDown() override {
        map_.reset();
        std::free(mem_);
    }
    MapEntry* mem_{nullptr};
    std::unique_ptr<OrderMap> map_;
};

TEST_F(OrderMapTest, InsertFind) {
    EXPECT_TRUE(map_->insert(100, 42));
    EXPECT_EQ(map_->find(100), 42u);
    EXPECT_EQ(map_->size(), 1u);
}

TEST_F(OrderMapTest, FindMissing) {
    EXPECT_EQ(map_->find(999), kMapEmpty);
}

TEST_F(OrderMapTest, Erase) {
    map_->insert(100, 42);
    EXPECT_TRUE(map_->erase(100));
    EXPECT_EQ(map_->find(100), kMapEmpty);
    EXPECT_EQ(map_->size(), 0u);
}

TEST_F(OrderMapTest, EraseMissing) {
    EXPECT_FALSE(map_->erase(999));
}

TEST_F(OrderMapTest, MultipleEntries) {
    // Insert 400 entries (40% load factor on 1024-cap map)
    for (uint32_t i = 1; i <= 400; ++i)
        EXPECT_TRUE(map_->insert(i * 1000ULL, i));

    for (uint32_t i = 1; i <= 400; ++i)
        EXPECT_EQ(map_->find(i * 1000ULL), i) << "find failed for key " << i * 1000;

    for (uint32_t i = 1; i <= 200; ++i)
        EXPECT_TRUE(map_->erase(i * 1000ULL));

    for (uint32_t i = 1; i <= 200; ++i)
        EXPECT_EQ(map_->find(i * 1000ULL), kMapEmpty);
    for (uint32_t i = 201; i <= 400; ++i)
        EXPECT_EQ(map_->find(i * 1000ULL), i);
}

TEST_F(OrderMapTest, ZeroKeyNeverValid) {
    // kEmptyKey=0 is the sentinel; inserting it would corrupt the table
    // (we validate that find(0) returns kMapEmpty when nothing inserted)
    EXPECT_EQ(map_->find(0), kMapEmpty);
}
