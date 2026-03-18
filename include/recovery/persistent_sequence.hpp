#pragma once
#include "../common/compiler.hpp"
#include "../concurrency/spsc_queue.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace itch {

// Returned by load() — zero-value means no state file found (cold start).
struct LoadedSeq {
    uint64_t seq        = 0;
    char     session[10] = {};
};

// -----------------------------------------------------------------
// PersistentSequence — durable last-sequence bookkeeper.
//
// File format (24 bytes, written atomically via tmp→rename):
//   char     magic[4]    = "ITCS"
//   char     session[10] — raw MoldUDP64 session, not null-terminated
//   uint16_t version     = 1
//   uint64_t seq         — last confirmed sequence, host byte order
//
// Hot path: enqueue(seq, session_10)
//   Single try_push onto a 2-slot SPSC queue.  Never blocks.
//   Drop-on-full is intentional: the background thread already holds
//   a pending save, so the on-disk record always advances.
//
// Cold path: background writer thread
//   Pops from the queue, calls save_sync() which does:
//     write PersistRecord → <path>.tmp
//     fdatasync
//     rename(<path>.tmp → <path>)       ← POSIX-atomic
//   Sleeps 1 ms when idle — not on the latency path.
// -----------------------------------------------------------------
class PersistentSequence {
  public:
    explicit PersistentSequence(std::string dir) noexcept;

    ~PersistentSequence() { stop(); }

    PersistentSequence(const PersistentSequence&)            = delete;
    PersistentSequence& operator=(const PersistentSequence&) = delete;

    // Cold path: read last state from <dir>/last_sequence.bin.
    // Must be called before start() and before any other workers run.
    LoadedSeq load() noexcept;

    // Hot path: non-blocking enqueue of (seq, 10-byte session) for the
    // background writer.  Returns immediately regardless of queue state.
    ITCH_FORCE_INLINE void enqueue(uint64_t seq, const char* session_10) noexcept {
        SeqRecord r{};          // zero-init pad bytes
        r.seq = seq;
        std::memcpy(r.session, session_10, 10);
        queue_.try_push(r);     // drop-on-full: the writer already has pending work
    }

    // Called by FeedHandler::start_workers() — launches background writer.
    void start();

    // Called by FeedHandler::stop_workers() AFTER parser_->stop() returns.
    // Drains queue and does a final synchronous save before joining.
    void stop() noexcept;

  private:
    // On-disk record (24 bytes, cache-line aligned access irrelevant here).
    struct PersistRecord {
        char     magic[4]    = {'I','T','C','S'};
        char     session[10] = {};
        uint16_t version     = 1;
        uint64_t seq         = 0;
    };
    static_assert(sizeof(PersistRecord) == 24);
    static_assert(std::is_trivially_copyable_v<PersistRecord>);

    // Queue payload — 24 bytes, power-of-2 sized, trivially copyable.
    struct SeqRecord {
        uint64_t seq;
        char     session[10];
        char     pad[6];
    };
    static_assert(sizeof(SeqRecord) == 24);
    static_assert(std::is_trivially_copyable_v<SeqRecord>);

    // 2-slot SPSC: double-buffered so the writer can consume slot N
    // while the parser queues slot N+1 during the fdatasync window.
    using Queue = SpscQueue<SeqRecord, 2>;

    std::string       dir_;
    std::string       path_;        // <dir>/last_sequence.bin
    Queue             queue_;
    std::atomic<bool> running_{false};
    std::thread       thread_;

    void run() noexcept;
    void save_sync(uint64_t seq, const char* session_10) noexcept;
};

} // namespace itch
