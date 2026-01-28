#pragma once
#include <cstddef>
#include <cstdint>

namespace itch {

constexpr std::size_t NUM_SHARDS = 8;
constexpr std::size_t MAX_ORDERS_PER_SHARD = 2'000'000;
constexpr std::size_t ORDER_MAP_CAPACITY = 4'194'304; // 2^22, must be power of 2
constexpr std::size_t SHARD_QUEUE_DEPTH = 262'144;    // 2^18
constexpr std::size_t PACKET_RING_DEPTH = 65'536;     // 2^16
constexpr std::size_t RECOVERY_RING_DEPTH = 1024;     // 2^10 — recovery back-pressure budget
// FeedMerger → ParserThread merged queue.  Same depth as a single ring so
// it can never be the bottleneck even when both feeds fire simultaneously.
constexpr std::size_t MERGED_QUEUE_DEPTH  = PACKET_RING_DEPTH; // 2^16
// Dedup sliding-window width (must be a multiple of 64).
// A/B feeds are microseconds apart; 128 sequences is far more than enough.
constexpr std::size_t DEDUP_WINDOW_SIZE   = 128;
constexpr std::size_t MAX_LOCATE = 65'536;
constexpr std::size_t CACHELINE_SIZE = 64;
constexpr std::size_t MAX_UDP_PAYLOAD = 1500;
// Compile-time maximum price levels per side per book.
// uint16_t so that the internal count fields can hold the full range 0..MAX_PRICE_LEVELS
// without uint8_t wrapping (which would silently corrupt books at the 256th level).
constexpr uint16_t MAX_PRICE_LEVELS = 256;
constexpr int64_t PRICE_SCALE = 10'000;
constexpr uint16_t MOLDUDP64_HEARTBEAT = 0xFFFF;

} // namespace itch
