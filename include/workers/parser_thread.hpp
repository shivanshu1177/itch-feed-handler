#pragma once
#include "../common/types.hpp"
#include "../monitoring/latency_histogram.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../network/packet_ring.hpp"
#include "../protocol/itch_parser.hpp"
#include "../protocol/moldudp64_parser.hpp"
#include "../protocol/sequence_tracker.hpp"
#include "../recovery/gap_detector.hpp"
#include "../recovery/persistent_sequence.hpp"
#include "../routing/shard_router.hpp"
#include "../session/session_manager.hpp"
#include "../system/cpu_affinity.hpp"
#include <atomic>
#include <thread>

namespace itch {

// -----------------------------------------------------------------
// ParserThread — hot-loop message parser and router.
//
// Consumes slot indices from the ZeroCopyRing, decodes MoldUDP64 +
// ITCH, tracks sequence numbers, and routes ItchMsg to shard queues.
// Uses a stack-allocated message buffer (no heap allocation).
// -----------------------------------------------------------------
class ParserThread {
  public:
    // primary_ring / secondary_ring: used only to resolve slot pointers.
    // FeedMerger is the sole producer of merged_queue; ParserThread is the consumer.
    ParserThread(ZeroCopyRing&        primary_ring,
                 ZeroCopyRing&        secondary_ring,  // buffer may be nullptr if disabled
                 MergedSlotQueue&     merged_queue,
                 ShardRouter&         router,
                 LatencyHistogram&    parse_lat,
                 FeedStats&           stats,
                 GapDetector&         gap_detector,
                 SequenceTracker&     seq_tracker,
                 SessionManager&      session_mgr,
                 PersistentSequence&  persist_seq,
                 const ThreadConfig&  cfg) noexcept
        : primary_ring_(primary_ring), secondary_ring_(secondary_ring),
          merged_queue_(merged_queue), router_(router),
          parse_lat_(parse_lat), stats_(stats),
          gap_detector_(gap_detector), seq_tracker_(seq_tracker),
          session_mgr_(session_mgr), persist_seq_(persist_seq), cfg_(cfg) {}

    ~ParserThread() { stop(); }

    void start();
    void stop();

  private:
    ZeroCopyRing&       primary_ring_;
    ZeroCopyRing&       secondary_ring_;
    MergedSlotQueue&    merged_queue_;
    ShardRouter&        router_;
    LatencyHistogram&   parse_lat_;
    FeedStats&          stats_;
    GapDetector&        gap_detector_;
    SequenceTracker&    seq_tracker_;   // owned by FeedHandler, shared with RecoveryThread
    SessionManager&     session_mgr_;
    PersistentSequence& persist_seq_;  // enqueue() on every in-order advance
    ThreadConfig        cfg_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    // Stack-allocated message buffer — zero heap allocation in hot path
    ItchMsg msg_buf_[ItchParser::MAX_MSGS_PER_PACKET];

    void run() noexcept;
    void process_slot(const PacketSlot* slot) noexcept;

    // Resolve a MergedSlot token to a const PacketSlot pointer.
    // Branch-free for the common Primary case (FeedSource::Primary == 0).
    ITCH_FORCE_INLINE const PacketSlot* resolve(const MergedSlot& ms) const noexcept {
        switch (ms.source) {
        case FeedSource::Primary:   return primary_ring_.slot(ms.slot_idx);
        case FeedSource::Secondary: return secondary_ring_.slot(ms.slot_idx);
        case FeedSource::Recovery:  return primary_ring_.recovery_slot(ms.slot_idx);
        }
        return primary_ring_.slot(ms.slot_idx); // unreachable
    }
};

} // namespace itch
