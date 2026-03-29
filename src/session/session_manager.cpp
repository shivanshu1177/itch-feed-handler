#include "session_manager.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/protocol/itch_messages.hpp"
#include "../../include/routing/shard_router.hpp" // full definition needed for broadcast()

namespace itch {

SessionManager::SessionManager(const char*      initial_session,
                                SequenceTracker& seq_tracker,
                                SymbolDirectory& sym_dir,
                                BboPublisher&    bbo_pub,
                                ShardRouter&     router,
                                bool             eod_archive_enabled) noexcept
    : eod_archive_enabled_(eod_archive_enabled),
      seq_tracker_(seq_tracker), sym_dir_(sym_dir),
      bbo_pub_(bbo_pub), router_(router) {
    std::memset(session_, 0, sizeof(session_));
    std::memset(watched_bits_, 0, sizeof(watched_bits_));
    if (initial_session &&
        std::strncmp(initial_session, "AUTO_DETECT", 11) != 0) {
        std::strncpy(session_, initial_session, 10);
        session_[10] = '\0';
    }
}

void SessionManager::register_eod_callback(EodCallback cb) noexcept {
    if (eod_callback_count_ < MAX_EOD_CALLBACKS) {
        eod_callbacks_[eod_callback_count_++] = std::move(cb);
    } else {
        ITCH_LOG_WARN("SessionManager: MAX_EOD_CALLBACKS (%zu) reached, callback dropped",
                      MAX_EOD_CALLBACKS);
    }
}

void SessionManager::add_watched_symbol(LocateCode loc) noexcept {
    if (loc < MAX_LOCATE) {
        watched_bits_[loc >> 6] |= (uint64_t{1} << (loc & 63u));
    }
}

void SessionManager::add_watched_ticker(const char* ticker8) noexcept {
    if (watched_ticker_count_ >= MAX_WATCHED_TICKERS) {
        ITCH_LOG_WARN("SessionManager: MAX_WATCHED_TICKERS (%zu) reached, ticker dropped",
                      MAX_WATCHED_TICKERS);
        return;
    }
    std::memcpy(watched_tickers_[watched_ticker_count_++], ticker8, 8);
}

SessionState SessionManager::event_code_to_state(char code) noexcept {
    switch (code) {
    case 'S': return SessionState::PreOpen;
    case 'O': return SessionState::Open;
    case 'C': return SessionState::Open;   // resume after operational halt
    case 'Q': return SessionState::Closed;
    case 'M': return SessionState::Closed;
    case 'E': return SessionState::Closed; // end of trade reporting
    default:  return SessionState::Unknown;
    }
}

void SessionManager::on_system_event(const ItchMsg& msg,
                                      const char*   raw_session_10,
                                      uint64_t      /*mold_seq*/) noexcept {
    // Session name change: trigger a full subsystem reset.
    if (raw_session_10 &&
        std::memcmp(session_, raw_session_10, 10) != 0 &&
        (session_[0] != '\0' || raw_session_10[0] != '\0')) {
        reset_session(raw_session_10);
    }

    char code = static_cast<char>(msg.system_event.event_code);
    SessionState new_state = event_code_to_state(code);
    if (new_state == SessionState::Unknown)
        return;

    // EOD archival fires on Q (End-of-Messages) or M (Emergency Halt)
    // before the state is committed to Closed, so callbacks still see
    // the current session name.
    if (new_state == SessionState::Closed && eod_archive_enabled_) {
        ITCH_LOG_INFO("SessionManager: EOD trigger event='%c' session='%.10s'",
                      code, session_);
        on_eod_archival();
    }

    state_.store(static_cast<uint8_t>(new_state), std::memory_order_release);
    ITCH_LOG_INFO("SessionManager: session='%.10s' event='%c' → state=%u",
                  session_, code, static_cast<unsigned>(new_state));
}

void SessionManager::on_eod_archival() noexcept {
    // Invoke every registered callback.  Each callback is typically
    // registered by FeedHandler and captures the shard's book pool +
    // BookSnapshot reference.
    for (std::size_t i = 0; i < eod_callback_count_; ++i) {
        if (eod_callbacks_[i]) {
            eod_callbacks_[i](nullptr, 0, session_);
        }
    }
}

void SessionManager::reset_session(const char* new_session_10) noexcept {
    char old_name[11];
    std::memcpy(old_name, session_, 10);
    old_name[10] = '\0';

    std::memcpy(session_, new_session_10, 10);
    session_[10] = '\0';

    ITCH_LOG_INFO("SessionManager: session '%.10s' → '%.10s'; resetting subsystems",
                  old_name, session_);

    seq_tracker_.reset();
    sym_dir_.reset();
    bbo_pub_.reset();

    ItchMsg reset_msg{};
    reset_msg.type = MessageType::InternalSessionReset;
    router_.broadcast(reset_msg);

    state_.store(static_cast<uint8_t>(SessionState::Unknown), std::memory_order_release);
}

} // namespace itch
