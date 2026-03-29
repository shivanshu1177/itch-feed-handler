#pragma once
#include "../common/types.hpp"
#include "../orderbook/bbo_publisher.hpp"
#include "../orderbook/order_book_pool.hpp"
#include "../protocol/sequence_tracker.hpp"
#include "../routing/symbol_directory.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace itch {

// Forward declarations break circular includes.
class ShardRouter;
class BookSnapshot;
struct ItchMsg;

// -----------------------------------------------------------------
// SessionState — mirrors ITCH system-event lifecycle.
//
// Transitions driven by SystemEvent event codes:
//   'S' → PreOpen   (start of hours)
//   'O' → Open      (start of market hours)
//   'C' → Open      (trading resumed after halt)
//   'Q' → Closed    (end of market hours / end of messages)
//   'M' → Closed    (emergency market close)
//   'E' → Closed    (end of trade reporting)
// -----------------------------------------------------------------
enum class SessionState : uint8_t {
    Unknown  = 0,
    PreOpen  = 1,
    Open     = 2,
    Closed   = 3,
};

// -----------------------------------------------------------------
// EodCallback — called on 'Q' or 'M' before the session is reset.
//
// The callback receives the book pool and book count (BOOKS_PER_SHARD)
// for the shard owned by the caller.  Because SessionManager has no
// direct access to book data (it lives in FeedHandler/ShardWorker),
// the callback is the decoupling point.
//
// Signature: void(const Level2Book* books, std::size_t count,
//                 const char* session_10)
// -----------------------------------------------------------------
using EodCallback = std::function<void(const Level2Book*, std::size_t, const char*)>;

// -----------------------------------------------------------------
// SessionManager — owns the ITCH session state machine.
//
// Called by ParserThread (hot path) on each SystemEvent message.
// Detects session name changes in the MoldUDP64 header and triggers
// a full subsystem reset.
//
// EOD archival:
//   'Q' / 'M' → on_eod_archival() is called (cold path) before
//   state transitions to Closed.  Registered callbacks do the actual
//   snapshot save; SessionManager never owns book data.
//
// Pre-open warmup:
//   Callers (e.g. replay tools, FeedHandler) may call
//   warmup_symbol(locate) during the PreOpen window to request that
//   a specific book be pre-populated from the last EOD snapshot.
//   is_warmup_symbol(locate) lets ShardWorker fast-reject symbols
//   that were not in the watch list.
//
// Zero heap allocation on the hot path.
// State is an atomic uint8_t — ShardWorker reads it lock-free.
// -----------------------------------------------------------------
class SessionManager {
  public:
    // initial_session: null-terminated string, or "AUTO_DETECT" to
    // populate from the first received MoldUDP64 header.
    // eod_archive_enabled: if false, on_eod_archival() is a no-op.
    SessionManager(const char*      initial_session,
                   SequenceTracker& seq_tracker,
                   SymbolDirectory& sym_dir,
                   BboPublisher&    bbo_pub,
                   ShardRouter&     router,
                   bool             eod_archive_enabled = true) noexcept;

    // Hot path: called by ParserThread for every SystemEvent message.
    void on_system_event(const ItchMsg& msg,
                         const char*   raw_session_10,
                         uint64_t      mold_seq) noexcept;

    // Register an EOD callback (cold-path setup, called before start()).
    // Multiple callbacks are supported (e.g. one per shard).
    // Thread-safe only before workers start — not meant to be called at
    // runtime.
    void register_eod_callback(EodCallback cb) noexcept;

    // Request that `locate` be treated as a watched symbol eligible for
    // pre-open warmup.  Idempotent.  Not thread-safe; call before start().
    void add_watched_symbol(LocateCode loc) noexcept;

    // Register a watched ticker by name (8-char space-padded ITCH format,
    // e.g. "AAPL    ").  Called in FeedHandler::init_workers() for each
    // entry in [session].watched_symbols.  Not thread-safe; call before start().
    // locate codes are not known at startup — they are resolved when the first
    // StockDirectory message for the ticker arrives (via try_resolve_watched).
    void add_watched_ticker(const char* ticker8) noexcept;

    // Called by ParserThread on every StockDirectory message.
    // Checks whether ticker8 matches any registered watched ticker; if so,
    // calls add_watched_symbol(loc) so ShardWorker activates the book during PreOpen.
    // Fast-exit when no watched tickers are registered (zero overhead).
    ITCH_FORCE_INLINE void try_resolve_watched(LocateCode loc,
                                               const char* ticker8) noexcept {
        if (ITCH_LIKELY(watched_ticker_count_ == 0))
            return;
        for (std::size_t i = 0; i < watched_ticker_count_; ++i) {
            if (std::memcmp(watched_tickers_[i], ticker8, 8) == 0) {
                add_watched_symbol(loc);
                return;
            }
        }
    }

    // Hot-path check used by ShardWorker: should this symbol be
    // activated during PreOpen for warmup?
    // Returns false when eod_archive_enabled_ is false or the symbol
    // was not in the watch list.
    ITCH_FORCE_INLINE bool is_warmup_symbol(LocateCode loc) const noexcept {
        if (!eod_archive_enabled_ || loc >= MAX_LOCATE)
            return false;
        // Flat bit-array: 1 bit per locate code.
        return (watched_bits_[loc >> 6] >> (loc & 63u)) & 1u;
    }

    // Lock-free gate used by ShardWorker on each StockDirectory and
    // order message.
    // memory_order_relaxed is sufficient: the SPSC queue provides the
    // required ordering for subsequent book operations.
    ITCH_FORCE_INLINE bool is_open() const noexcept {
        return static_cast<SessionState>(state_.load(std::memory_order_relaxed))
               == SessionState::Open;
    }

    ITCH_FORCE_INLINE bool is_pre_open() const noexcept {
        return static_cast<SessionState>(state_.load(std::memory_order_relaxed))
               == SessionState::PreOpen;
    }

    // Current session name (null-terminated, max 10 chars + null).
    const char* current_session() const noexcept { return session_; }

    SessionState state() const noexcept {
        return static_cast<SessionState>(state_.load(std::memory_order_relaxed));
    }

  private:
    char                 session_[11];
    std::atomic<uint8_t> state_{static_cast<uint8_t>(SessionState::Unknown)};
    bool                 eod_archive_enabled_;

    SequenceTracker& seq_tracker_;
    SymbolDirectory& sym_dir_;
    BboPublisher&    bbo_pub_;
    ShardRouter&     router_;

    // One callback per shard (avoids heap allocation).
    static constexpr std::size_t MAX_EOD_CALLBACKS = NUM_SHARDS;
    EodCallback  eod_callbacks_[MAX_EOD_CALLBACKS];
    std::size_t  eod_callback_count_{0};

    // Bit-array for watched symbols: 65536 bits = 1024 uint64_t = 8KB.
    // Set by add_watched_symbol(); read lock-free by is_warmup_symbol().
    uint64_t watched_bits_[MAX_LOCATE / 64]{};

    // Ticker strings for watched symbols (8-char, not null-terminated).
    // Resolved to locate codes by try_resolve_watched() when StockDirectory arrives.
    static constexpr std::size_t MAX_WATCHED_TICKERS = 256;
    char        watched_tickers_[MAX_WATCHED_TICKERS][8]{};
    std::size_t watched_ticker_count_{0};

    // Cold path: triggers on session name change.
    void reset_session(const char* new_session_10) noexcept;

    // Cold path: called on 'Q' or 'M' before state→Closed.
    void on_eod_archival() noexcept;

    static SessionState event_code_to_state(char code) noexcept;
};

} // namespace itch
