#pragma once
#include "../common/compiler.hpp"
#include "../common/constants.hpp"
#include "cacheline.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>

namespace itch {

template <typename T, std::size_t Capacity> class SpscQueue {
  public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    SpscQueue() = default;
    ~SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    ITCH_FORCE_INLINE bool try_push(const T& item) {
        std::size_t cached_read = cached_read_pos_;
        std::size_t write = write_pos_.load(std::memory_order_relaxed);
        if (write - cached_read >= Capacity) {
            cached_read = read_pos_.load(std::memory_order_acquire);
            cached_read_pos_ = cached_read;
            if (write - cached_read >= Capacity) {
                return false;
            }
        }
        buffer_[write & (Capacity - 1)] = item;
        write_pos_.store(write + 1, std::memory_order_release);
        return true;
    }

    ITCH_FORCE_INLINE bool try_pop(T& item) {
        std::size_t cached_write = cached_write_pos_;
        std::size_t read = read_pos_.load(std::memory_order_relaxed);
        if (read == cached_write) {
            cached_write = write_pos_.load(std::memory_order_acquire);
            cached_write_pos_ = cached_write;
            if (read == cached_write) {
                return false;
            }
        }
        item = buffer_[read & (Capacity - 1)];
        read_pos_.store(read + 1, std::memory_order_release);
        return true;
    }

    ITCH_FORCE_INLINE std::size_t size_approx() const {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

    // Returns true when fill level ≥ high_watermark entries.
    // Uses relaxed loads — intentionally racy; this is a soft pressure hint,
    // not a correctness check.
    ITCH_FORCE_INLINE bool is_pressure_high(std::size_t high_watermark) const noexcept {
        std::size_t w = write_pos_.load(std::memory_order_relaxed);
        std::size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w - r) >= high_watermark;
    }

  private:
    alignas(64) std::atomic<std::size_t> write_pos_{0};
    alignas(64) std::size_t cached_read_pos_{0};
    alignas(64) T buffer_[Capacity];
    alignas(64) std::size_t cached_write_pos_{0};
    alignas(64) std::atomic<std::size_t> read_pos_{0};
};

} // namespace itch
