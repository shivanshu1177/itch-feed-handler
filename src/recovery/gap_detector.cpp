#include "gap_detector.hpp"
#include "../../include/common/logging.hpp"
#include "../../include/common/timestamp.hpp"

namespace itch {

void GapDetector::on_gap(const GapInfo& info) noexcept {
    gap_.start_seq      = info.start_seq;
    gap_.end_seq        = info.end_seq;
    gap_.detected_at_ns = info.detected_at_ns;
    gap_.recovery_sent  = false;
    pending_            = true;
    ITCH_LOG_WARN("GapDetector: gap [%lu, %lu] detected at %lu ns",
                  gap_.start_seq, gap_.end_seq, gap_.detected_at_ns);
}

void GapDetector::mark_recovered(uint64_t up_to_seq) noexcept {
    if (pending_ && up_to_seq >= gap_.end_seq) {
        ITCH_LOG_INFO("GapDetector: gap [%lu, %lu] recovered",
                      gap_.start_seq, gap_.end_seq);
        pending_ = false;
        gap_     = {};
    }
}

void GapDetector::mark_recovery_sent() noexcept {
    gap_.recovery_sent = true;
}

bool GapDetector::should_request_recovery(Timestamp now_ns) const noexcept {
    if (!pending_ || gap_.recovery_sent)
        return false;
    return (now_ns - gap_.detected_at_ns) >= gap_timeout_ns_;
}

} // namespace itch
