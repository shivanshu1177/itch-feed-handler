#pragma once
#include "../common/constants.hpp"
#include <utility>

namespace itch {

constexpr std::size_t kCacheLineSize = CACHELINE_SIZE;

template <typename T> struct alignas(kCacheLineSize) CacheAligned {
    T value{};
    CacheAligned() = default;
    template <typename... Args>
    explicit CacheAligned(Args&&... args) : value(std::forward<Args>(args)...) {}
    operator T&() { return value; }
    operator const T&() const { return value; }
    T& operator*() { return value; }
    const T& operator*() const { return value; }
    T* operator->() { return &value; }
    const T* operator->() const { return &value; }
};

} // namespace itch
