#pragma once
#include "../common/compiler.hpp"
#include "cacheline.hpp"
#include <atomic>
#include <type_traits>

namespace itch {

// SeqLock: wait-free for readers, exclusive for writer.
// Writer: fetch_add(1) → odd (writer active) → write data → store(seq+2) → even (complete)
// Reader: spin while seq is odd, read data, retry if seq changed
template <typename T> class SeqLock {
  public:
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    SeqLock() = default;
    ~SeqLock() = default;
    SeqLock(const SeqLock&) = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    ITCH_FORCE_INLINE void store(const T& value) {
        // seq goes: even → odd (writer active) → even+2 (write complete)
        // acq_rel on the fetch_add prevents the data write from being
        // reordered before the seq increment on ARM64 (the 'acquire' half
        // of acq_rel acts as a two-way barrier for subsequent operations).
        // On x86 this is a no-op overhead (all RMWs are already full fences).
        auto seq = seq_.fetch_add(1, std::memory_order_acq_rel); // now odd
        data_.val = value;
        seq_.store(seq + 2, std::memory_order_release); // advance to next even
    }

    ITCH_FORCE_INLINE T load() const {
        T value;
        while (true) {
            uint32_t seq1 = seq_.load(std::memory_order_acquire);
            if (seq1 % 2 == 1)
                continue; // writer active, spin
            value = data_.val;
            // Acquire fence: on ARM64 this emits dmb ish, preventing the
            // plain loads of data_.val from being reordered AFTER the seq2
            // load below. Without this fence, the CPU can speculatively
            // advance seq2 ahead of the data reads, making torn reads
            // invisible to the consistency check.
            std::atomic_thread_fence(std::memory_order_acquire);
            uint32_t seq2 = seq_.load(std::memory_order_relaxed); // fence above provides ordering
            if (seq1 == seq2) {
                return value; // consistent read
            }
            // seq changed during read — retry
        }
    }

  private:
    alignas(64) mutable std::atomic<uint32_t> seq_{0};
    struct DataStore { T val{}; };
    alignas(64) DataStore data_;
};

} // namespace itch
