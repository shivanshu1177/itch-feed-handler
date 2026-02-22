#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "../concurrency/seqlock.hpp"
#include "level2_book.hpp"
#include <memory>

namespace itch {

// -----------------------------------------------------------------
// BboPublisher — publishes best bid/offer updates via SeqLock.
//
// One SeqLock<BboSnapshot> per locate code (up to MAX_LOCATE=65536).
// Each SeqLock is 128 bytes (64B seq_ + 64B data_).
// Total memory: 65536 * 128 = 8MB — fits in LLC on modern CPUs.
//
// Writer (ShardWorker): publish() after every book update
// Readers (strategy/risk): read() — wait-free, no locking
// -----------------------------------------------------------------
class BboPublisher {
  public:
    explicit BboPublisher(std::size_t max_symbols = MAX_LOCATE);

    // Called by ShardWorker after every BBO change — single writer per locate
    ITCH_FORCE_INLINE void publish(LocateCode loc, const BboSnapshot& bbo) noexcept {
        if (ITCH_UNLIKELY(loc >= capacity_))
            return;
        slots_[loc].store(bbo);
    }

    // Called by downstream consumers — wait-free multi-reader
    ITCH_FORCE_INLINE BboSnapshot read(LocateCode loc) const noexcept {
        if (ITCH_UNLIKELY(loc >= capacity_))
            return BboSnapshot{};
        return slots_[loc].load();
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Zero all BBO slots (called by SessionManager on session change).
    void reset() noexcept;

  private:
    std::unique_ptr<SeqLock<BboSnapshot>[]> slots_;
    std::size_t capacity_;
};

} // namespace itch
