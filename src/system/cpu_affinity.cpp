#include "cpu_affinity.hpp"
#include "../../include/common/logging.hpp"
#include <cstdlib>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
// NUMA support (optional — enabled if libnuma is present)
#ifdef HAVE_NUMA
#include <numa.h>
#endif
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace itch {

bool CpuAffinity::pin_current_thread(int cpu_id) noexcept {
    if (cpu_id < 0)
        return true; // no-op

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        ITCH_LOG_WARN("pthread_setaffinity_np(cpu=%d) failed: errno=%d", cpu_id, rc);
        return false;
    }
    ITCH_LOG_INFO("Thread pinned to CPU %d", cpu_id);
    return true;
#elif defined(__APPLE__)
    // macOS: use thread_policy_set with THREAD_AFFINITY_POLICY
    thread_affinity_policy_data_t policy = {cpu_id + 1}; // tag, not strict binding
    kern_return_t kr = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_AFFINITY_POLICY,
        (thread_policy_t)&policy,
        THREAD_AFFINITY_POLICY_COUNT);
    if (kr != KERN_SUCCESS) {
        ITCH_LOG_WARN("thread_policy_set(cpu=%d) failed: %d (affinity hints only on macOS)",
                      cpu_id, kr);
        return false;
    }
    ITCH_LOG_INFO("Thread affinity hint set to tag %d (CPU %d) on macOS", cpu_id + 1, cpu_id);
    return true;
#else
    ITCH_LOG_WARN("CPU pinning not supported on this platform");
    return false;
#endif
}

bool CpuAffinity::set_realtime(int priority) noexcept {
    if (priority <= 0)
        return true; // no-op

#ifdef __linux__
    struct sched_param param{};
    param.sched_priority = priority;
    int rc = sched_setscheduler(0, SCHED_FIFO, &param);
    if (rc != 0) {
        ITCH_LOG_WARN("sched_setscheduler(SCHED_FIFO, prio=%d) failed: requires CAP_SYS_NICE",
                      priority);
        return false;
    }
    ITCH_LOG_INFO("Real-time scheduling SCHED_FIFO priority=%d enabled", priority);
    return true;
#else
    ITCH_LOG_WARN("Real-time scheduling (SCHED_FIFO) not supported on this platform");
    return false;
#endif
}

bool CpuAffinity::apply(const ThreadConfig& cfg) noexcept {
    bool ok = true;
    if (cfg.cpu_id >= 0)
        ok &= pin_current_thread(cfg.cpu_id);
    if (cfg.sched_policy == 1 || cfg.sched_policy == 2)
        ok &= set_realtime(cfg.sched_priority);
    return ok;
}

void* CpuAffinity::alloc_on_numa(std::size_t size, int numa_node) noexcept {
#if defined(__linux__) && defined(HAVE_NUMA)
    if (numa_available() >= 0 && numa_node >= 0) {
        void* ptr = numa_alloc_onnode(size, numa_node);
        if (ptr)
            return ptr;
    }
#endif
#if defined(__linux__)
    // mmap + mbind fallback
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return nullptr;
    madvise(ptr, size, MADV_HUGEPAGE);
    // mbind if numa_node specified (optional, non-fatal if missing)
    return ptr;
#else
    (void)numa_node;
    return std::aligned_alloc(64, size);
#endif
}

void CpuAffinity::free_numa(void* ptr, std::size_t size) noexcept {
    if (!ptr)
        return;
#if defined(__linux__) && defined(HAVE_NUMA)
    if (numa_available() >= 0) {
        numa_free(ptr, size);
        return;
    }
    munmap(ptr, size);
#elif defined(__linux__)
    munmap(ptr, size);
#else
    (void)size;
    std::free(ptr);
#endif
}

int CpuAffinity::numa_node_of_cpu(int cpu_id) noexcept {
#if defined(__linux__) && defined(HAVE_NUMA)
    if (numa_available() >= 0)
        return numa_node_of_cpu(cpu_id);
#endif
    (void)cpu_id;
    return -1;
}

int CpuAffinity::cpu_count() noexcept {
    return static_cast<int>(std::thread::hardware_concurrency());
}

} // namespace itch
