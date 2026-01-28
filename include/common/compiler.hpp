#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define ITCH_LIKELY(x) __builtin_expect(!!(x), 1)
#define ITCH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define ITCH_FORCE_INLINE __attribute__((always_inline)) inline
#define ITCH_NOINLINE __attribute__((noinline))
#define ITCH_HOT __attribute__((hot))
#define ITCH_COLD __attribute__((cold))
#define ITCH_PACKED __attribute__((packed))
#define ITCH_PREFETCH_R(ptr) __builtin_prefetch((ptr), 0, 3)
#define ITCH_PREFETCH_W(ptr) __builtin_prefetch((ptr), 1, 3)
#else
#define ITCH_LIKELY(x) (x)
#define ITCH_UNLIKELY(x) (x)
#define ITCH_FORCE_INLINE inline
#define ITCH_NOINLINE
#define ITCH_HOT
#define ITCH_COLD
#define ITCH_PACKED
#define ITCH_PREFETCH_R(ptr)
#define ITCH_PREFETCH_W(ptr)
#endif

namespace itch {
// No namespace content needed, just macros
}
