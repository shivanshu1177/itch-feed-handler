#include "order_map.hpp"
#include "../../include/common/logging.hpp"
#include <cstring>

namespace itch {

OrderMap::OrderMap(MapEntry* table, uint32_t capacity) noexcept
    : table_(table), capacity_(capacity), mask_(capacity - 1), size_(0) {
    // Capacity must be power of 2
    std::memset(table_, 0, capacity * sizeof(MapEntry));
}

void OrderMap::reset() noexcept {
    std::memset(table_, 0, capacity_ * sizeof(MapEntry));
    size_ = 0;
    ITCH_LOG_INFO("OrderMap reset: capacity=%u", capacity_);
}

bool OrderMap::insert(OrderId id, uint32_t pool_idx) noexcept {
    if (ITCH_UNLIKELY(size_ >= capacity_ * 3 / 4)) {
        ITCH_LOG_WARN("OrderMap near capacity: size=%u cap=%u", size_, capacity_);
    }

    MapEntry to_insert{id, pool_idx, 0, 0};
    uint32_t slot = hash_slot(id);

    while (true) {
        MapEntry& e = table_[slot & mask_];

        if (e.key == kEmptyKey) {
            // Empty slot: place here
            e = to_insert;
            ++size_;
            return true;
        }

        if (e.key == id) {
            // Key already present: update value (order replace scenario)
            e.value = pool_idx;
            return true;
        }

        // Robin Hood: if existing entry has shorter probe distance, swap
        if (e.probe_dist < to_insert.probe_dist) {
            MapEntry tmp = e;
            e = to_insert;
            to_insert = tmp;
        }

        ++to_insert.probe_dist;
        ++slot;
    }
}

uint32_t OrderMap::find(OrderId id) const noexcept {
    uint32_t slot = hash_slot(id);
    uint16_t dist = 0;

    while (true) {
        const MapEntry& e = table_[slot & mask_];

        if (e.key == kEmptyKey || dist > e.probe_dist)
            return kMapEmpty; // not found

        if (e.key == id)
            return e.value;

        ++dist;
        ++slot;
    }
}

bool OrderMap::erase(OrderId id) noexcept {
    uint32_t slot = hash_slot(id);
    uint16_t dist = 0;

    // Find the entry
    while (true) {
        MapEntry& e = table_[slot & mask_];
        if (e.key == kEmptyKey || dist > e.probe_dist)
            return false; // not found
        if (e.key == id)
            break;
        ++dist;
        ++slot;
    }

    // Backward shift: move subsequent entries one slot back
    uint32_t pos = slot;
    while (true) {
        uint32_t next = (pos + 1) & mask_;
        MapEntry& cur  = table_[pos & mask_];
        MapEntry& next_e = table_[next];

        if (next_e.key == kEmptyKey || next_e.probe_dist == 0) {
            cur = MapEntry{};
            break;
        }
        cur = next_e;
        --cur.probe_dist;
        pos = next;
    }

    --size_;
    return true;
}

} // namespace itch
