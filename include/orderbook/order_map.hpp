#pragma once
#include "../common/compiler.hpp"
#include "../common/types.hpp"
#include <cstdint>

namespace itch {

// -----------------------------------------------------------------
// OrderMap — Robin Hood open-addressing hash table.
//
// Maps OrderId (uint64_t) → pool index (uint32_t).
// Capacity = ORDER_MAP_CAPACITY = 4M (2^22), load factor ~50%.
//
// Robin Hood hashing: on insert, if the probe distance of the
// existing entry is shorter than the inserting entry's probe
// distance, swap them. This keeps average probe length low even at
// high load and eliminates tombstones on erase (backward shift).
//
// Entry size: 16 bytes → total table = 4M * 16 = 64MB (fits in LLC).
// -----------------------------------------------------------------

static constexpr uint32_t kMapEmpty     = UINT32_MAX;
static constexpr OrderId  kEmptyKey     = 0; // OrderId 0 is never valid in ITCH

struct alignas(16) MapEntry {
    OrderId  key        = kEmptyKey; // 8 bytes
    uint32_t value      = 0;         // 4 bytes — pool index
    uint16_t probe_dist = 0;         // 2 bytes — Robin Hood probe distance
    uint16_t pad        = 0;         // 2 bytes
};
static_assert(sizeof(MapEntry) == 16);

class OrderMap {
  public:
    // table must point to capacity * sizeof(MapEntry) zeroed bytes, 16-byte aligned
    OrderMap(MapEntry* table, uint32_t capacity) noexcept;

    // Insert OrderId → pool_idx. Returns true on success.
    bool insert(OrderId id, uint32_t pool_idx) noexcept;

    // Lookup. Returns pool index, or kMapEmpty if not found.
    uint32_t find(OrderId id) const noexcept;

    // Erase. Returns true if the key was present. Uses backward shift (no tombstones).
    bool erase(OrderId id) noexcept;

    uint32_t size()     const noexcept { return size_; }
    uint32_t capacity() const noexcept { return capacity_; }

    // Wipe all entries (called by ShardWorker on InternalSessionReset).
    void reset() noexcept;

  private:
    MapEntry* table_;
    uint32_t  capacity_; // power of 2
    uint32_t  mask_;     // capacity - 1
    uint32_t  size_;

    ITCH_FORCE_INLINE uint32_t hash_slot(OrderId id) const noexcept {
        // Fibonacci hashing: multiply by 2^64/phi, take top bits
        return static_cast<uint32_t>(
            (id * 11400714819323198485ULL) >> (64 - __builtin_ctz(capacity_)));
    }
};

} // namespace itch
