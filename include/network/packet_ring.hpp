#pragma once
#include "../common/constants.hpp"
#include "../common/types.hpp"
#include "../concurrency/spsc_queue.hpp"
#include "packet.hpp"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <sys/mman.h>
#endif

namespace itch {

// -----------------------------------------------------------------
// Zero-copy packet ring
//
// The receiver writes directly into pre-allocated PacketSlot entries
// (no memcpy). Only a 2-byte slot index is passed through the SPSC
// queue, keeping the queue itself small (128KB — fits in L2).
//
// Producer (ReceiverThread):
//   1. idx = write_slot & (DEPTH-1)
//   2. Write directly into ring.claim(idx) via recvfrom/recvmsg
//   3. ring.publish(idx); ++write_slot;
//
// Consumer (ParserThread):
//   1. ring.consume(idx)
//   2. const PacketSlot* s = ring.slot(idx)  ← zero-copy pointer
//   3. Parse from s (no copy needed)
//
// Slot reuse is safe: SPSC guarantees consumer always reads before
// the producer wraps back to the same index.
// -----------------------------------------------------------------

// One cache-line aligned slot — filled directly by recvfrom/recvmsg
struct alignas(64) PacketSlot {
    uint8_t   data[MAX_UDP_PAYLOAD]; // 1500 bytes
    uint16_t  length;                // bytes received
    uint16_t  pad0{0};
    uint32_t  pad1{0};
    Timestamp recv_ts{0};            // NIC HW or software timestamp (ns)
};
static_assert(sizeof(PacketSlot) % 64 == 0 || sizeof(PacketSlot) < 64);

// The pre-allocated array of slots (backed by huge pages when available)
struct PacketRingBuffer {
    PacketSlot slots[PACKET_RING_DEPTH];
};

// SPSC queue carries only 16-bit slot indices
// Total queue memory: PACKET_RING_DEPTH * sizeof(uint16_t) = 128KB
using SlotIndexQueue = SpscQueue<uint16_t, PACKET_RING_DEPTH>;

// Separate recovery SPSC — same index type, smaller capacity.
// RecoveryThread is the sole producer; FeedMerger is the sole consumer.
// Keeping it separate from SlotIndexQueue preserves SPSC discipline on both paths.
using RecoverySlotQueue = SpscQueue<uint16_t, RECOVERY_RING_DEPTH>;

// -----------------------------------------------------------------
// A/B feed merge types
// -----------------------------------------------------------------

// Identifies which ring a MergedSlot came from.
enum class FeedSource : uint8_t { Primary = 0, Secondary = 1, Recovery = 2 };

// 4-byte token forwarded from FeedMerger to ParserThread.
// No data copy: ParserThread resolves the pointer from the originating ring.
struct MergedSlot {
    uint16_t   slot_idx{0};
    FeedSource source{FeedSource::Primary};
    uint8_t    pad{0};
};
static_assert(sizeof(MergedSlot) == 4);
static_assert(std::is_trivially_copyable_v<MergedSlot>);

// FeedMerger (producer) → ParserThread (consumer)
using MergedSlotQueue = SpscQueue<MergedSlot, MERGED_QUEUE_DEPTH>;

// -----------------------------------------------------------------
// Allocate the ring buffer.
// Tries MAP_HUGETLB (2MB pages) first on Linux; falls back to
// regular mmap with MADV_HUGEPAGE (transparent huge pages).
// -----------------------------------------------------------------
inline PacketRingBuffer* alloc_packet_ring_buffer() {
    constexpr std::size_t size = sizeof(PacketRingBuffer);

#if defined(__linux__)
#  if defined(MAP_HUGETLB)
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (ptr != MAP_FAILED) {
        return static_cast<PacketRingBuffer*>(ptr);
    }
#  endif
    // Regular mmap + transparent huge pages
    void* ptr2 = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr2 != MAP_FAILED) {
        madvise(ptr2, size, MADV_HUGEPAGE);
        return static_cast<PacketRingBuffer*>(ptr2);
    }
#endif
    // POSIX fallback (macOS, etc.)
    void* fb = std::aligned_alloc(64, size);
    return static_cast<PacketRingBuffer*>(fb);
}

inline void free_packet_ring_buffer(PacketRingBuffer* buf) noexcept {
#if defined(__linux__)
    munmap(buf, sizeof(PacketRingBuffer));
#else
    std::free(buf);
#endif
}

// -----------------------------------------------------------------
// ZeroCopyRing — thin wrapper combining buffer pointer + index queue
// -----------------------------------------------------------------
struct ZeroCopyRing {
    PacketRingBuffer* buffer = nullptr; // not owned; use alloc/free above

    SlotIndexQueue queue;

    // Producer: claim the write slot for direct filling
    PacketSlot* claim(uint16_t idx) noexcept {
        return &buffer->slots[idx & static_cast<uint16_t>(PACKET_RING_DEPTH - 1)];
    }

    // Producer: publish a filled slot index to the consumer
    bool publish(uint16_t idx) noexcept { return queue.try_push(idx); }

    // Consumer: retrieve the next available slot index
    bool consume(uint16_t& idx) noexcept { return queue.try_pop(idx); }

    // Consumer: get a read-only pointer to a slot by index
    const PacketSlot* slot(uint16_t idx) const noexcept {
        return &buffer->slots[idx & static_cast<uint16_t>(PACKET_RING_DEPTH - 1)];
    }

    std::size_t depth_approx() const noexcept { return queue.size_approx(); }

    // -----------------------------------------------------------------
    // Recovery injection — RecoveryThread → ParserThread path.
    //
    // Uses a separate SPSC queue + slot buffer so RecoveryThread never
    // touches SlotIndexQueue (which is single-producer for ReceiverThread).
    //
    // inject_recovered_packet() — call from RecoveryThread only.
    //   Checks capacity BEFORE claiming the slot so we never write into a
    //   slot the consumer (ParserThread) is currently reading.
    //   Returns true on success; false + increments drop_ctr on overflow.
    // -----------------------------------------------------------------
    bool inject_recovered_packet(const uint8_t* data, uint16_t len,
                                  Timestamp recv_ts,
                                  std::atomic<uint64_t>& drop_ctr,
                                  std::atomic<uint64_t>& recovered_ctr) noexcept {
        // Conservative capacity check before claiming the slot.
        // Since this is SPSC (we are the only writer), size_approx() is safe:
        // write_pos is our own counter (exact); read_pos may be slightly stale
        // (consumer advanced it). Stale-low means we might see "full" when
        // there is actually one free slot — we drop unnecessarily but safely.
        if (recovery_queue.size_approx() >= RECOVERY_RING_DEPTH) {
            drop_ctr.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        uint16_t slot_idx = recovery_write_ & static_cast<uint16_t>(RECOVERY_RING_DEPTH - 1);
        PacketSlot& s = recovery_slots[slot_idx];
        s.length  = len;
        s.recv_ts = recv_ts;
        std::memcpy(s.data, data, len);
        if (!recovery_queue.try_push(slot_idx)) {
            // Between size_approx and try_push the queue filled (extremely rare).
            // The slot write above is safe: consumer cannot have reached slot_idx
            // because the queue was not yet full when we checked.
            drop_ctr.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ++recovery_write_;
        recovered_ctr.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Consumer (ParserThread only): get next recovered slot index.
    bool consume_recovered(uint16_t& idx) noexcept { return recovery_queue.try_pop(idx); }

    // Consumer: pointer to a recovery slot by index.
    const PacketSlot* recovery_slot(uint16_t idx) const noexcept {
        return &recovery_slots[idx & static_cast<uint16_t>(RECOVERY_RING_DEPTH - 1)];
    }

  private:
    // Separate slot buffer for recovered packets — not backed by huge pages
    // (recovery is background; ~1.5 MB, not latency-critical).
    PacketSlot        recovery_slots[RECOVERY_RING_DEPTH]{};
    RecoverySlotQueue recovery_queue;
    uint16_t          recovery_write_{0}; // single writer (RecoveryThread) — non-atomic
};

} // namespace itch
