#include "order_pool.hpp"
#include "../../include/common/logging.hpp"

namespace itch {

OrderPool::OrderPool(uint32_t capacity, void* backing_memory) noexcept
    : pool_(static_cast<Order*>(backing_memory)),
      capacity_(capacity),
      freelist_(nullptr),
      freelist_top_(capacity) {
    // Freelist is embedded at the END of the backing memory, after the Order array.
    // Caller must ensure backing_memory has capacity*sizeof(Order) + capacity*sizeof(uint32_t).
    // In practice, FeedHandler allocates combined pool.
    freelist_ = reinterpret_cast<uint32_t*>(pool_ + capacity);

    // Pre-fill: slot 0 is first allocation (stack grows down from top)
    for (uint32_t i = 0; i < capacity; ++i) {
        freelist_[i] = capacity - 1 - i; // slot 0 comes out first
    }
    ITCH_LOG_INFO("OrderPool created: capacity=%u, memory=%zu MB",
                  capacity, (capacity * (sizeof(Order) + sizeof(uint32_t))) / (1024 * 1024));
}

Order* OrderPool::alloc() noexcept {
    if (ITCH_UNLIKELY(freelist_top_ == 0)) {
        ITCH_LOG_ERROR("OrderPool exhausted! capacity=%u", capacity_);
        return nullptr;
    }
    uint32_t idx = freelist_[--freelist_top_];
    allocated_.fetch_add(1, std::memory_order_relaxed);
    Order* o = &pool_[idx];
    o->prev_idx = kInvalidIdx;
    o->next_idx = kInvalidIdx;
    return o;
}

void OrderPool::reset() noexcept {
    freelist_top_ = capacity_;
    for (uint32_t i = 0; i < capacity_; ++i)
        freelist_[i] = capacity_ - 1 - i;
    allocated_.store(0, std::memory_order_relaxed);
    ITCH_LOG_INFO("OrderPool reset: capacity=%u", capacity_);
}

void OrderPool::free(uint32_t idx) noexcept {
    freelist_[freelist_top_++] = idx;
    allocated_.fetch_sub(1, std::memory_order_relaxed);
}

} // namespace itch
