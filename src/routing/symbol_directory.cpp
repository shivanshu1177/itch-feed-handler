#include "symbol_directory.hpp"
#include "../../include/common/logging.hpp"
#include <cstring>

namespace itch {

void SymbolDirectory::register_symbol(LocateCode loc, const char* stock8,
                                       uint32_t lot_size, uint8_t market_cat,
                                       uint8_t fin_status) noexcept {
    if (loc == 0 || loc >= MAX_LOCATE) {
        ITCH_LOG_WARN("SymbolDirectory: invalid locate code %u", loc);
        return;
    }

    SymbolInfo& s = symbols_[loc];
    // Copy 8-byte stock field from wire, null-terminate
    std::memcpy(s.ticker, stock8, 8);
    s.ticker[8]       = '\0';
    s.locate          = loc;
    s.shard           = static_cast<ShardId>(loc % NUM_SHARDS);
    s.market_category = market_cat;
    s.financial_status = fin_status;
    s.round_lot_size  = lot_size;
    s.is_active       = true;

    count_.fetch_add(1, std::memory_order_relaxed);
    ITCH_LOG_DEBUG("Registered symbol: %.8s locate=%u shard=%u",
                   stock8, loc, s.shard);
}

void SymbolDirectory::reset() noexcept {
    for (auto& s : symbols_)
        s.is_active = false;
    count_.store(0, std::memory_order_relaxed);
    ITCH_LOG_INFO("SymbolDirectory reset: all %zu symbols deactivated", symbols_.size());
}

} // namespace itch
