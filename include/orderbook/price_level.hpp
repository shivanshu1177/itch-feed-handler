#pragma once
#include "../common/types.hpp"
#include <cstdint>

namespace itch {

// -----------------------------------------------------------------
// PriceLevel — one cache line per price point.
//
// Orders at the same price form a doubly-linked list via pool indices
// (head_order_idx → … → tail_order_idx).
//
// bid_levels_[0]  = best bid  (highest price)
// ask_levels_[0]  = best ask  (lowest price)
// Both arrays are kept sorted best-first at all times.
// -----------------------------------------------------------------
struct alignas(64) PriceLevel {
    Price    price           = 0;          // 8
    Qty      total_qty       = 0;          // 4
    uint32_t order_count     = 0;          // 4
    uint32_t head_order_idx  = UINT32_MAX; // 4  pool index of first order (FIFO)
    uint32_t tail_order_idx  = UINT32_MAX; // 4  pool index of last order (O(1) append)
    uint32_t pad[10]         = {};         // 40  → total 64 bytes
};
static_assert(sizeof(PriceLevel) == 64);

} // namespace itch
