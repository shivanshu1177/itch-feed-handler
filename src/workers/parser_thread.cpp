#include "parser_thread.hpp"
#include "../../include/common/logging.hpp"
#include <thread>

namespace itch {

void ParserThread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ParserThread::run, this);
    ITCH_LOG_INFO("ParserThread started (cpu=%d)", cfg_.cpu_id);
}

void ParserThread::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

void ParserThread::run() noexcept {
    CpuAffinity::apply(cfg_);

    uint32_t packet_count = 0;

    while (running_.load(std::memory_order_acquire)) {
        // Consume from FeedMerger's merged queue.
        // FeedMerger already prioritises recovery slots (FeedSource::Recovery)
        // and has deduped A/B traffic, so we just dispatch whatever arrives.
        MergedSlot ms;
        if (merged_queue_.try_pop(ms)) {
            process_slot(resolve(ms));

            // Soft back-pressure: every 100 packets, check if any shard queue
            // is above the pressure threshold. If so, yield once to give
            // ShardWorkers a chance to drain. On SCHED_FIFO this is advisory —
            // the kernel may not preempt immediately, but it signals intent.
            if (ITCH_UNLIKELY(++packet_count >= 100)) {
                packet_count = 0;
                for (ShardId s = 0; s < NUM_SHARDS; ++s) {
                    if (router_.is_pressure_high(s)) {
                        std::this_thread::yield();
                        break; // one yield per check cycle is sufficient
                    }
                }
            }
        }
        // Empty queue: spin (no sleep on hot path)
    }
}

void ParserThread::process_slot(const PacketSlot* slot) noexcept {
    // 1. Parse MoldUDP64 transport header
    auto mold = MoldUDP64Parser::parse(slot->data, slot->length);
    if (!mold.valid)
        return;

    if (mold.header.is_heartbeat())
        return; // keep-alive, no messages

    // 2. Track sequence number
    uint64_t seq = mold.header.get_sequence();
    bool in_order = seq_tracker_.try_advance(seq);
    if (in_order) {
        // Non-blocking: try_push to background writer; drop-on-full is fine.
        // session field comes directly from the wire header — no copy overhead.
        persist_seq_.enqueue(seq, mold.header.session);
        // Clear pending gap once all missing sequences have been re-injected
        // and processed in order. Called on every in-order advance — cheap
        // because mark_recovered() is a no-op when there is no pending gap.
        gap_detector_.mark_recovered(seq);
    } else {
        // Gap or duplicate
        if (seq_tracker_.gap_count() > 0) {
            GapInfo gi = seq_tracker_.last_gap();
            gap_detector_.on_gap(gi);
            stats_.sequence_gaps.fetch_add(1, std::memory_order_relaxed);
        } else {
            stats_.sequence_dups.fetch_add(1, std::memory_order_relaxed);
        }
        // Continue processing the packet even if out-of-order (best effort)
    }

    // 3. Parse ITCH messages — zero allocation (stack buffer)
    Timestamp recv_ts = slot->recv_ts;
    int count = ItchParser::parse(slot->data, slot->length, recv_ts,
                                   mold, msg_buf_, ItchParser::MAX_MSGS_PER_PACKET);

    // 4. Route each message to the appropriate shard queue
    for (int i = 0; i < count; ++i) {
        // Timestamp the latency from wire-receive to parse-complete
        Timestamp parse_ts = steady_ns();
        if (recv_ts > 0 && parse_ts >= recv_ts) {
            parse_lat_.record(parse_ts - recv_ts);
        }

        // Forward SystemEvent messages to SessionManager before routing
        // so that session state is updated before shard workers see
        // subsequent StockDirectory messages.
        if (msg_buf_[i].type == MessageType::SystemEvent) {
            session_mgr_.on_system_event(msg_buf_[i], mold.header.session, seq);
        }

        // Resolve watched-symbol locate codes BEFORE routing.
        // The SPSC queue's release fence (in router_.route) ensures ShardWorker
        // sees the watched bit set when it dequeues the StockDirectory message.
        if (msg_buf_[i].type == MessageType::StockDirectory) {
            session_mgr_.try_resolve_watched(msg_buf_[i].locate,
                                             msg_buf_[i].stock_directory.stock);
        }

        (void)router_.route(msg_buf_[i]); // drop counter incremented inside ShardRouter
        stats_.messages_parsed.fetch_add(1, std::memory_order_relaxed);

        // Count per-type
        uint8_t type_idx = static_cast<uint8_t>(msg_buf_[i].type);
        stats_.msg_type_count[type_idx].fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace itch
