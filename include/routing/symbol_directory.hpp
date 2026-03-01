#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace itch {

struct SymbolInfo {
    char       ticker[9]     = {};  // null-terminated, 8 chars + null (ITCH stock field)
    LocateCode locate        = 0;
    ShardId    shard         = 0;   // locate % NUM_SHARDS
    uint8_t    market_category = 0;
    uint32_t   round_lot_size  = 100;
    uint8_t    financial_status = 0;
    bool       is_active     = false;
};

// -----------------------------------------------------------------
// SymbolDirectory — direct-indexed array, O(1) lookup by locate code.
//
// Populated by StockDirectory messages at start-of-day.
// Locate code 0 is never valid; indices are 1-based in ITCH.
// Total memory: 65536 * sizeof(SymbolInfo) ≈ 2MB — fits in L2/L3.
// -----------------------------------------------------------------
class SymbolDirectory {
  public:
    // Register a symbol from a StockDirectory message.
    void register_symbol(LocateCode loc, const char* stock8,
                         uint32_t lot_size, uint8_t market_cat,
                         uint8_t fin_status) noexcept;

    // O(1) lookup — returns nullptr for unknown locate codes
    ITCH_FORCE_INLINE const SymbolInfo* lookup(LocateCode loc) const noexcept {
        if (loc == 0 || loc >= MAX_LOCATE)
            return nullptr;
        const SymbolInfo& s = symbols_[loc];
        return s.is_active ? &s : nullptr;
    }

    // Direct shard assignment (stable for the session)
    ITCH_FORCE_INLINE ShardId shard_for_locate(LocateCode loc) const noexcept {
        return static_cast<ShardId>(loc % NUM_SHARDS);
    }

    std::size_t symbol_count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    // Mark all symbols inactive (called by SessionManager on session change).
    // Symbols are re-populated by incoming StockDirectory messages.
    void reset() noexcept;

  private:
    std::array<SymbolInfo, MAX_LOCATE> symbols_{};
    std::atomic<std::size_t> count_{0};
};

} // namespace itch
