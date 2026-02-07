#pragma once
#include <cstddef>
#include <cstdint>

namespace itch {

struct ThreadConfig {
    int cpu_id        = -1; // -1 = no pinning
    int sched_policy  = 0;  // SCHED_OTHER=0, SCHED_FIFO=1, SCHED_RR=2
    int sched_priority = 0; // 1-99 for SCHED_FIFO/RR
    int numa_node     = -1; // -1 = no NUMA preference
};

class CpuAffinity {
  public:
    // Pin the calling thread to a specific CPU core.
    // Returns true on success, false if not supported or permission denied.
    static bool pin_current_thread(int cpu_id) noexcept;

    // Set real-time scheduling policy (SCHED_FIFO) on the calling thread.
    // Requires CAP_SYS_NICE or root. Logs a warning on EPERM but does not abort.
    static bool set_realtime(int priority) noexcept;

    // Apply a full ThreadConfig (affinity + scheduler) to the calling thread.
    static bool apply(const ThreadConfig& cfg) noexcept;

    // Allocate size bytes of memory preferentially on numa_node.
    // Falls back to malloc if NUMA is unavailable.
    // Caller must free with free_numa().
    static void* alloc_on_numa(std::size_t size, int numa_node) noexcept;
    static void  free_numa(void* ptr, std::size_t size) noexcept;

    // Return the NUMA node of a given CPU core (-1 if unknown).
    static int numa_node_of_cpu(int cpu_id) noexcept;

    // Number of logical CPUs available.
    static int cpu_count() noexcept;
};

} // namespace itch
