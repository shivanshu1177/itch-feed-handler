#include "bbo_publisher.hpp"
#include "../../include/common/logging.hpp"

namespace itch {

BboPublisher::BboPublisher(std::size_t max_symbols)
    : slots_(std::make_unique<SeqLock<BboSnapshot>[]>(max_symbols)),
      capacity_(max_symbols) {
    ITCH_LOG_INFO("BboPublisher created: %zu slots, %.1f MB",
                  max_symbols,
                  static_cast<double>(max_symbols * sizeof(SeqLock<BboSnapshot>)) /
                      (1024.0 * 1024.0));
}

void BboPublisher::reset() noexcept {
    for (std::size_t i = 0; i < capacity_; ++i)
        slots_[i].store(BboSnapshot{});
    ITCH_LOG_INFO("BboPublisher reset: %zu slots zeroed", capacity_);
}

} // namespace itch
