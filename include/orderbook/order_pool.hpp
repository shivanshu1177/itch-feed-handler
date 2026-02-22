#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace itch {

// -----------------------------------------------------------------
// Order — core book entry (doubly-linked within price level)
// 32 bytes = 2 per cache line → high cache utilization
// -----------------------------------------------------------------
struct alignas(32) Order {
    OrderId  id;          // 8  order reference number
    Price    price;       // 8  fixed-point ×10000 (host byte order)
    Qty      qty;         // 4  current quantity remaining
    uint32_t prev_idx;    // 4  pool index of previous order in level (UINT32_MAX = head)
    uint32_t next_idx;    // 4  pool index of next order in level (UINT32_MAX = tail)
    uint16_t level_idx;   // 2  index into bid_levels_ or ask_levels_ array
    Side     side;        // 1
    uint8_t  pad;         // 1
};
static_assert(sizeof(Order) == 32);
static_assert(std::is_trivially_destructible_v<Order>);

static constexpr uint32_t kInvalidIdx = UINT32_MAX;

// -----------------------------------------------------------------
// OrderPool — slab allocator backed by huge-page memory.
// Zero heap allocation on alloc/free in steady state.
// -----------------------------------------------------------------
class OrderPool {
  public:
    // capacity must be <= MAX_ORDERS_PER_SHARD
    // backing_memory must be capacity * sizeof(Order) bytes, 64-byte aligned
    OrderPool(uint32_t capacity, void* backing_memory) noexcept;

    // Allocate one Order. Returns nullptr if pool is exhausted.
    Order* alloc() noexcept;

    // Return an Order to the pool by its pool index.
    void free(uint32_t idx) noexcept;

    // Pool index of an Order pointer (for embedding in doubly-linked lists)
    ITCH_FORCE_INLINE uint32_t index_of(const Order* o) const noexcept {
        return static_cast<uint32_t>(o - pool_);
    }

    ITCH_FORCE_INLINE Order* at(uint32_t idx) noexcept {
        return &pool_[idx];
    }
    ITCH_FORCE_INLINE const Order* at(uint32_t idx) const noexcept {
        return &pool_[idx];
    }

    uint32_t allocated() const noexcept { return allocated_.load(std::memory_order_relaxed); }
    uint32_t capacity()  const noexcept { return capacity_; }

    // Rebuild the freelist to initial state (called by ShardWorker on
    // InternalSessionReset). O(capacity) but happens only at session boundary.
    void reset() noexcept;

  private:
    Order*   pool_;
    uint32_t capacity_;
    // LIFO free stack: freelist_[freelist_top_-1] is the next allocation index
    uint32_t* freelist_;
    uint32_t  freelist_top_;
    std::atomic<uint32_t> allocated_{0};
};

} // namespace itch
