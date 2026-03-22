#include "persistent_sequence.hpp"
#include "../../include/common/logging.hpp"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace itch {

PersistentSequence::PersistentSequence(std::string dir) noexcept
    : dir_(std::move(dir)),
      path_(dir_ + "/last_sequence.bin") {}

LoadedSeq PersistentSequence::load() noexcept {
    int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            ITCH_LOG_INFO("PersistentSequence: no state file at %s — cold start (seq=0)",
                          path_.c_str());
        else
            ITCH_LOG_WARN("PersistentSequence: open(%s) failed: %s",
                          path_.c_str(), strerror(errno));
        return {};
    }

    PersistRecord rec;
    ssize_t n = ::read(fd, &rec, sizeof(rec));
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(rec))) {
        ITCH_LOG_WARN("PersistentSequence: truncated state file (%zd/%zu bytes) — cold start",
                      n, sizeof(rec));
        return {};
    }
    if (std::memcmp(rec.magic, "ITCS", 4) != 0) {
        ITCH_LOG_WARN("PersistentSequence: bad magic in state file — cold start");
        return {};
    }

    LoadedSeq r;
    r.seq = rec.seq;
    std::memcpy(r.session, rec.session, 10);
    ITCH_LOG_INFO("PersistentSequence: loaded seq=%lu session='%.10s'", r.seq, r.session);
    return r;
}

void PersistentSequence::save_sync(uint64_t seq, const char* session_10) noexcept {
    PersistRecord rec;
    // magic and version are set by PersistRecord's default member initialisers
    std::memcpy(rec.session, session_10, 10);
    rec.version = 1;
    rec.seq     = seq;

    // Atomic write pattern: write tmp → fdatasync → rename.
    // rename(2) is atomic on POSIX within the same filesystem — readers
    // never see a partial write.
    std::string tmp = path_ + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ITCH_LOG_ERROR("PersistentSequence: open(%s) failed: %s",
                       tmp.c_str(), strerror(errno));
        return;
    }

    ssize_t n = ::write(fd, &rec, sizeof(rec));
    ::fsync(fd);        // flush data before rename so the new inode is durable
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(rec))) {
        ITCH_LOG_ERROR("PersistentSequence: short write (%zd/%zu)", n, sizeof(rec));
        return;
    }
    if (::rename(tmp.c_str(), path_.c_str()) != 0) {
        ITCH_LOG_ERROR("PersistentSequence: rename(%s→%s) failed: %s",
                       tmp.c_str(), path_.c_str(), strerror(errno));
    }
}

void PersistentSequence::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&PersistentSequence::run, this);
    ITCH_LOG_INFO("PersistentSequence writer started, path=%s", path_.c_str());
}

void PersistentSequence::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return; // already stopped or never started

    if (thread_.joinable())
        thread_.join(); // wait for writer to exit its loop

    // Producer (ParserThread) is stopped before this — safe to drain.
    // Any item that arrived between the writer's last try_pop and the
    // loop exit is flushed here for a precise on-disk final state.
    SeqRecord r;
    if (queue_.try_pop(r))
        save_sync(r.seq, r.session);

    ITCH_LOG_INFO("PersistentSequence writer stopped");
}

void PersistentSequence::run() noexcept {
    using namespace std::chrono_literals;
    SeqRecord r;

    while (running_.load(std::memory_order_acquire)) {
        if (queue_.try_pop(r)) {
            save_sync(r.seq, r.session);
            // Drain any item that queued during the fdatasync window so
            // the on-disk record catches up quickly on bursty traffic.
            while (queue_.try_pop(r))
                save_sync(r.seq, r.session);
        } else {
            // Idle — yield the core for 1 ms.  This thread is intentionally
            // off the latency path; tight-spinning here would waste a core.
            std::this_thread::sleep_for(1ms);
        }
    }
}

} // namespace itch
