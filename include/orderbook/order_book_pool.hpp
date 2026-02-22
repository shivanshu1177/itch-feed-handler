#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "level2_book.hpp"
#include <cstddef>

namespace itch {

class OrderPool;
class OrderMap;

// -----------------------------------------------------------------
// OrderBookPool — pre-allocated slab of Level2Book objects.
//
// One pool per ShardWorker.  Capacity = MAX_LOCATE / NUM_SHARDS = 8192.
//
// Slab index mapping (for locate codes owned by shard_id):
//   slab_index = loc / NUM_SHARDS
//
// Proof: for shard s, valid locate codes are {s, s+8, s+16, …}.
//   loc = s + i*NUM_SHARDS  →  loc / NUM_SHARDS = i  (integer division)
//   Inverse: loc = slab_index * NUM_SHARDS + shard_id
//
// All Level2Book objects are placement-new'd at construction with the
// correct locate code, pool, and map references.  book() is O(1) and
// branch-free on the hot path.
//
// Memory: BOOKS_PER_SHARD × sizeof(Level2Book)
//         = 8192 × 32896 ≈ 257 MiB per shard (NUMA-local, huge-page).
//         Total across 8 shards ≈ 2.0 GiB.
//         Replaces the previous lazy heap allocation (new Level2Book)
//         that caused latency spikes when SOD StockDirectory bursts
//         interleaved with early order messages.
//
// Lifecycle:
//   1. FeedHandler::allocate_memory() constructs one pool per shard.
//   2. ShardWorker calls book(loc) + clear() to activate a locate slot;
//      never calls new/delete.
//   3. Pool destructor calls free_numa() — no explicit Level2Book dtors
//      needed because Level2Book is trivially destructible.
// -----------------------------------------------------------------
class OrderBookPool {
  public:
    // MAX_LOCATE / NUM_SHARDS locate codes per shard (most inactive).
    static constexpr std::size_t BOOKS_PER_SHARD = MAX_LOCATE / NUM_SHARDS;

    // Allocates BOOKS_PER_SHARD * sizeof(Level2Book) bytes via
    // CpuAffinity::alloc_on_numa(size, numa_node), then placement-news
    // each Level2Book with its correct locate code.
    OrderBookPool(ShardId    shard_id,
                  OrderPool& order_pool,
                  OrderMap&  order_map,
                  int        numa_node) noexcept;

    // Frees the slab. No explicit Level2Book destructors called
    // (Level2Book is trivially destructible).
    ~OrderBookPool() noexcept;

    OrderBookPool(const OrderBookPool&)            = delete;
    OrderBookPool& operator=(const OrderBookPool&) = delete;

    // O(1) slab lookup — no allocation, no branch.
    // Caller MUST only invoke this for locate codes where
    //   loc % NUM_SHARDS == shard_id.
    ITCH_FORCE_INLINE Level2Book* book(LocateCode loc) noexcept {
        return &slab_[loc / NUM_SHARDS];
    }

    // Pointer to the start of the book slab — for snapshot serialisation.
    const Level2Book* slab() const noexcept { return slab_; }

    bool        ok()            const noexcept { return slab_ != nullptr; }
    std::size_t backing_bytes() const noexcept { return mem_bytes_; }

  private:
    ShardId     shard_id_;
    void*       mem_{nullptr};
    Level2Book* slab_{nullptr};
    std::size_t mem_bytes_{0};
};

} // namespace itch
