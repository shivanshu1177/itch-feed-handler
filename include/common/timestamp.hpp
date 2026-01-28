#pragma once
#include "compiler.hpp"
#include "types.hpp"
#include <chrono>
#include <cstdint>

#if defined(__aarch64__) && defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace itch {

// Portable monotonic clock — used for durations and timeouts
inline uint64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------
// Platform-specific low-latency wallclock (nanoseconds since epoch).
// On Linux x86_64: calibrated RDTSC (sub-nanosecond resolution).
// On Apple ARM64:  hardware CNTVCT converted via mach_timebase_info.
// Fallback:        clock_gettime(CLOCK_REALTIME).
// ---------------------------------------------------------------------

#if defined(__x86_64__)

namespace detail {
struct TscCalibration {
    uint64_t numer; // multiply TSC ticks by numer
    uint64_t denom; // then divide by denom  → nanoseconds
};

inline TscCalibration calibrate_tsc() noexcept {
    // 1. Try CPUID leaf 0x15: TSC frequency = ecx * ebx / eax  (Hz)
    //    ns = tsc * (1e9 * eax) / (ecx * ebx)  →  numer=1e9*eax, denom=ecx*ebx
#if defined(__GNUC__) || defined(__clang__)
    {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15));
        if (eax != 0 && ebx != 0 && ecx != 0) {
            // TSC Hz = ecx * ebx / eax
            // ns = tsc / (ecx * ebx / eax) * 1e9
            //    = tsc * eax * 1e9 / (ecx * ebx)
            uint64_t n = static_cast<uint64_t>(eax) * 1'000'000'000ULL;
            uint64_t d = static_cast<uint64_t>(ecx) * static_cast<uint64_t>(ebx);
            return {n, d};
        }
    }
#endif
    // 2. Fallback: measure TSC ticks over a ~10ms wall-clock window
    {
        // Warm up
        struct timespec ts0, ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        uint64_t tsc0;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax"
                         : "=a"(tsc0)
                         :
                         : "rdx");

        // Wait ~10ms
        do {
            clock_gettime(CLOCK_MONOTONIC, &ts1);
        } while ((ts1.tv_sec - ts0.tv_sec) * 1'000'000'000LL +
                     (ts1.tv_nsec - ts0.tv_nsec) <
                 10'000'000LL);

        uint64_t tsc1;
        __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax"
                         : "=a"(tsc1)
                         :
                         : "rdx");

        uint64_t elapsed_ns = static_cast<uint64_t>(ts1.tv_sec - ts0.tv_sec) * 1'000'000'000ULL +
                              static_cast<uint64_t>(ts1.tv_nsec - ts0.tv_nsec);
        uint64_t tsc_ticks = tsc1 - tsc0;
        // ns = tsc * elapsed_ns / tsc_ticks
        return {elapsed_ns, tsc_ticks};
    }
}

// Lazy-initialized at first call, written once (effectively const after startup)
inline const TscCalibration& tsc_cal() noexcept {
    static TscCalibration cal = calibrate_tsc();
    return cal;
}
} // namespace detail

inline uint64_t now_ns() noexcept {
    uint64_t tsc;
    __asm__ volatile("rdtsc; shl $32, %%rdx; or %%rdx, %%rax" : "=a"(tsc) : : "rdx");
    const auto& c = detail::tsc_cal();
    // Use __uint128_t to avoid overflow in the multiply
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(tsc) * c.numer) / c.denom);
}

#elif defined(__aarch64__) && defined(__APPLE__)

inline uint64_t now_ns() noexcept {
    // mach_timebase_info: multiply by numer/denom to get nanoseconds
    static const auto timebase = []() {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info; // numer/denom is typically 1/1 on Apple Silicon
    }();
    uint64_t cntvct;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cntvct));
    // For Apple ARM: CNTVCT is generally not directly convertible without CNTFRQ.
    // Use mach_absolute_time() which is already in the correct timebase.
    // Fall through to mach_absolute_time() which is the recommended API.
    (void)cntvct;
    uint64_t t = mach_absolute_time();
    return (t * static_cast<uint64_t>(timebase.numer)) / static_cast<uint64_t>(timebase.denom);
}

#else // Generic POSIX fallback

inline uint64_t now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

#endif

} // namespace itch
