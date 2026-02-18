#include "order_book_pool.hpp"
#include "../system/cpu_affinity.hpp"
#include "../common/logging.hpp"
#include <new>

namespace itch {

OrderBookPool::OrderBookPool(ShardId    shard_id,
                              OrderPool& order_pool,
                              OrderMap&  order_map,
                              int        numa_node) noexcept
    : shard_id_(shard_id) {
    mem_bytes_ = BOOKS_PER_SHARD * sizeof(Level2Book);
    mem_ = CpuAffinity::alloc_on_numa(mem_bytes_, numa_node);
    if (!mem_) {
        ITCH_LOG_ERROR("OrderBookPool[%u]: failed to allocate %zu bytes on NUMA node %d",
                       static_cast<unsigned>(shard_id_), mem_bytes_, numa_node);
        mem_bytes_ = 0;
        return;
    }

    slab_ = static_cast<Level2Book*>(mem_);

    // Placement-new each book with its correct locate code.
    // Inverse formula: loc = slab_index * NUM_SHARDS + shard_id
    for (std::size_t i = 0; i < BOOKS_PER_SHARD; ++i) {
        LocateCode loc = static_cast<LocateCode>(i * NUM_SHARDS + shard_id_);
        new(&slab_[i]) Level2Book(loc, order_pool, order_map);
    }

    ITCH_LOG_INFO("OrderBookPool[%u]: %zu books, %.1f MiB on NUMA node %d",
                  static_cast<unsigned>(shard_id_), BOOKS_PER_SHARD,
                  static_cast<double>(mem_bytes_) / (1024.0 * 1024.0),
                  numa_node);
}

OrderBookPool::~OrderBookPool() noexcept {
    if (!mem_)
        return;
    // Level2Book is trivially destructible — no explicit dtor loop needed.
    CpuAffinity::free_numa(mem_, mem_bytes_);
    mem_  = nullptr;
    slab_ = nullptr;
}

} // namespace itch
