#include "shard_worker.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/common/timestamp.hpp"
#include "../../include/protocol/itch_messages.hpp"
#include <cstring>

namespace itch {

ShardWorker::ShardWorker(ShardId shard_id, ShardQueue& queue, BboPublisher& bbo_pub,
                         LatencyHistogram& book_lat, const SymbolDirectory& sym_dir,
                         OrderPool& pool, OrderMap& map, OrderBookPool& book_pool,
                         const SessionManager& session_mgr, FeedStats& stats,
                         const ThreadConfig& cfg) noexcept
    : shard_id_(shard_id), queue_(queue), bbo_pub_(bbo_pub), book_lat_(book_lat),
      sym_dir_(sym_dir), pool_(pool), map_(map), book_pool_(book_pool),
      session_mgr_(session_mgr), stats_(stats), cfg_(cfg) {
    std::memset(book_active_, 0, sizeof(book_active_));
}

void ShardWorker::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ShardWorker::run, this);
    ITCH_LOG_INFO("ShardWorker[%u] started (cpu=%d)", shard_id_, cfg_.cpu_id);
}

void ShardWorker::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
    // No book ownership — OrderBookPool destructor frees the slab.
}

void ShardWorker::run() noexcept {
    CpuAffinity::apply(cfg_);

    ItchMsg msg;
    while (running_.load(std::memory_order_acquire)) {
        if (queue_.try_pop(msg)) {
            dispatch(msg);
        }
    }
}

Level2Book* ShardWorker::activate_book(LocateCode loc, const char* stock8) noexcept {
    std::size_t idx = loc / NUM_SHARDS;
    if (!book_active_[idx]) {
        book_active_[idx] = true;
        // Attach per-shard counters once at activation — pointers never change.
        book_pool_.book(loc)->set_drop_ctr(&stats_.price_level_drops[shard_id_]);
        book_pool_.book(loc)->set_inplace_ctr(&stats_.order_replaces_inplace[shard_id_]);
        ITCH_LOG_INFO("ShardWorker[%u]: activated book for locate=%u stock=%.8s",
                      shard_id_, loc, stock8 ? stock8 : "????????");
    }
    return book_pool_.book(loc);
}

void ShardWorker::clear_all_books() noexcept {
    for (std::size_t i = 0; i < OrderBookPool::BOOKS_PER_SHARD; ++i) {
        if (book_active_[i]) {
            LocateCode loc = static_cast<LocateCode>(i * NUM_SHARDS + shard_id_);
            book_pool_.book(loc)->clear();
            book_active_[i] = false;
        }
    }
    ITCH_LOG_INFO("ShardWorker[%u]: all books cleared", shard_id_);
}

void ShardWorker::dispatch(const ItchMsg& msg) noexcept {
    // Internal sentinel from SessionManager::reset_session()
    if (msg.type == MessageType::InternalSessionReset) {
        clear_all_books();
        pool_.reset();
        map_.reset();
        return;
    }

    // StockDirectory: activate the pre-allocated book.
    //   • Open    → normal SOD activation.
    //   • PreOpen → activate only if this symbol is in the warmup watch list
    //               so the book is ready when replayed EOD prices arrive.
    //   • Closed/Unknown → discard (stale traffic).
    if (msg.type == MessageType::StockDirectory) {
        SessionState st = session_mgr_.state();
        if (st == SessionState::Open) {
            activate_book(msg.locate, msg.stock_directory.stock);
        } else if (st == SessionState::PreOpen &&
                   session_mgr_.is_warmup_symbol(msg.locate)) {
            activate_book(msg.locate, msg.stock_directory.stock);
        }
        return;
    }

    if (msg.type == MessageType::StockTradingAction) {
        if (book_active_[msg.locate / NUM_SHARDS])
            book_pool_.book(msg.locate)->on_trading_action(msg);
        return;
    }

    // Gate ALL order messages on session being Open.
    // During PreOpen or Closed the order flow is not yet valid;
    // silently discard rather than corrupt book state.
    if (ITCH_UNLIKELY(!session_mgr_.is_open()))
        return;

    if (ITCH_UNLIKELY(!book_active_[msg.locate / NUM_SHARDS])) {
        // Locate not yet registered — late AddOrder before StockDirectory
        // (should not happen in normal ITCH flow after SOD).
        if (msg.type == MessageType::AddOrder || msg.type == MessageType::AddOrderMPID) {
            activate_book(msg.locate, msg.add_order.stock);
        } else {
            return; // discard
        }
    }

    Level2Book* book = book_pool_.book(msg.locate);

    Timestamp before = steady_ns();

    switch (msg.type) {
    case MessageType::AddOrder:
    case MessageType::AddOrderMPID:
        book->on_add_order(msg);
        break;
    case MessageType::OrderExecuted:
        book->on_order_executed(msg);
        break;
    case MessageType::OrderExecutedPrice:
        book->on_order_executed_price(msg);
        break;
    case MessageType::OrderCancel:
        book->on_order_cancel(msg);
        break;
    case MessageType::OrderDelete:
        book->on_order_delete(msg);
        break;
    case MessageType::OrderReplace:
        book->on_order_replace(msg);
        break;
    default:
        return; // non-book messages already handled above
    }

    // Publish updated BBO
    BboSnapshot bbo = book->get_bbo();
    bbo.ts = msg.timestamp;
    bbo_pub_.publish(msg.locate, bbo);

    // Record wire-to-book latency
    Timestamp after = steady_ns();
    if (msg.recv_timestamp > 0 && after >= msg.recv_timestamp) {
        book_lat_.record(after - msg.recv_timestamp);
    }
    (void)before;
}

} // namespace itch
