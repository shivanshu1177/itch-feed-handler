#pragma once
#include "../common/constants.hpp"
#include "../monitoring/latency_histogram.hpp"
#include "../monitoring/metrics_reporter.hpp"
#include "../network/packet_ring.hpp"
#include "../orderbook/bbo_publisher.hpp"
#include "../orderbook/order_book_pool.hpp"
#include "../orderbook/order_map.hpp"
#include "../orderbook/order_pool.hpp"
#include "../protocol/sequence_tracker.hpp"
#include "../recovery/book_snapshot.hpp"
#include "../recovery/gap_detector.hpp"
#include "../recovery/persistent_sequence.hpp"
#include "../routing/shard_router.hpp"
#include "../routing/symbol_directory.hpp"
#include "../session/session_manager.hpp"
#include "../system/cpu_affinity.hpp"
#include "../workers/feed_merger.hpp"
#include "../workers/parser_thread.hpp"
#include "../workers/receiver_thread.hpp"
#include "../workers/recovery_thread.hpp"
#include "../workers/shard_worker.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace itch {

// Configuration loaded from TOML file
struct FeedHandlerConfig {
    // [network]
    std::string multicast_group = "233.54.12.111";
    uint16_t    port            = 26477;
    std::string interface       = "0.0.0.0";
    // Secondary (B-feed) — leave multicast_group_secondary empty to disable.
    std::string multicast_group_secondary = {};
    uint16_t    port_secondary            = 0;
    std::string interface_secondary       = "0.0.0.0";
    uint64_t    failover_timeout_ms       = 5000;

    // [threads]
    int receiver_core   = 1;
    int parser_core     = 2;
    std::array<int, NUM_SHARDS> shard_cores{3,4,5,6,7,8,9,10};
    int recovery_core   = 11;
    int merger_core     = 12; // FeedMerger thread (SCHED_FIFO 77)

    // [monitoring]
    uint32_t report_interval_sec  = 10;

    // [recovery]
    uint64_t    gap_timeout_ms        = 500;
    uint32_t    snapshot_interval_sec = 60;
    // Primary retransmit server (NASDAQ SoupTCP / SGX equivalent).
    // 198.51.100.1 is the RFC 5737 documentation placeholder — startup
    // validation rejects it with exit(1).
    std::string retransmit_address    = "198.51.100.1";
    uint16_t    retransmit_port       = 17001;
    // Secondary retransmit server — optional dual-feed / cross-connect.
    // Leave address empty or port 0 to disable.
    std::string retransmit_address_secondary = {};
    uint16_t    retransmit_port_secondary    = 0;
    // Login credentials for SoupBinTCP handshake.
    // require_login = false → skip the handshake (unauthenticated test feeds).
    std::string retransmit_username = {};
    std::string retransmit_password = {};
    bool        require_login       = false;
    std::string snapshot_dir               = "/var/lib/itch/snapshots";
    std::string snapshot_filename_template = "itch_{session}_{timestamp}.snap";

    // [session]
    // "AUTO_DETECT": populated from the first received MoldUDP64 session field.
    // Explicit value (e.g. "STRT00000T") seeds the tracker without waiting.
    std::string initial_session = "AUTO_DETECT";
    // true → save_eod() on 'Q' / 'M' SystemEvent before state→Closed.
    bool        eod_archive_enabled = true;
    // Symbols to pre-load from prior-day EOD snapshot during PreOpen.
    // Each entry is an 8-char ITCH ticker (space-padded), e.g. "AAPL    ".
    std::vector<std::string> watched_symbols{};

    // [persistence]
    std::string last_sequence_dir = "/var/lib/itch";

    // [sharding]
    // Fraction of SHARD_QUEUE_DEPTH at which ParserThread yields to let
    // ShardWorkers drain. Range: (0.0, 1.0]. Default: 80%.
    double queue_full_alert_threshold = 0.8;
};

FeedHandlerConfig load_config(const std::string& path);

// -----------------------------------------------------------------
// FeedHandler — top-level orchestrator.
//
// Owns all subsystems and coordinates their lifecycle.
// -----------------------------------------------------------------
class FeedHandler {
  public:
    explicit FeedHandler(const std::string& config_path);
    ~FeedHandler();

    bool init() noexcept;
    void run();   // blocks until stop() is called
    void stop();

    // Public BBO access (for downstream consumers / strategies)
    BboPublisher& bbo_publisher() noexcept { return *bbo_pub_; }

  private:
    FeedHandlerConfig cfg_;

    // Primary ring (A-feed)
    PacketRingBuffer* ring_buf_{nullptr};
    ZeroCopyRing      ring_;

    // Secondary ring (B-feed) — buffer is nullptr when secondary is disabled
    PacketRingBuffer* secondary_ring_buf_{nullptr};
    ZeroCopyRing      secondary_ring_;

    // FeedMerger → ParserThread channel
    MergedSlotQueue   merged_queue_;

    // Per-shard memory (pool + map + book pool + latency histogram)
    struct ShardMem {
        void*    order_pool_mem = nullptr;
        void*    order_map_mem  = nullptr;
        std::unique_ptr<OrderPool>     pool;
        std::unique_ptr<OrderMap>      map;
        std::unique_ptr<OrderBookPool> book_pool;
        LatencyHistogram               book_lat;
    };
    std::array<ShardMem, NUM_SHARDS> shard_mem_;

    // Core subsystems
    std::unique_ptr<SymbolDirectory> sym_dir_;
    std::unique_ptr<BboPublisher>    bbo_pub_;
    std::unique_ptr<ShardRouter>     router_;

    // Monitoring
    FeedStats         stats_;
    LatencyHistogram  parse_lat_;
    std::unique_ptr<MetricsReporter> metrics_;

    // Session tracking (owned here; shared by reference with ParserThread,
    // RecoveryThread, and SessionManager)
    SequenceTracker                 seq_tracker_;
    std::unique_ptr<SessionManager> session_mgr_;

    // Sequence persistence — background writer, enqueued from ParserThread
    std::unique_ptr<PersistentSequence> persist_seq_;

    // Per-shard EOD snapshots — each saves BOOKS_PER_SHARD books to a
    // shard-specific file derived from [recovery].snapshot_filename_template
    // with "_sN" inserted before the extension (e.g. "itch_..._s0.snap").
    std::array<std::unique_ptr<BookSnapshot>, NUM_SHARDS> eod_snapshots_;

    // Recovery
    GapDetector       gap_detector_;

    // Workers
    std::unique_ptr<ReceiverThread>                      receiver_;
    std::unique_ptr<ReceiverThread>                      secondary_receiver_; // nullptr if disabled
    std::unique_ptr<FeedMerger>                          feed_merger_;
    std::unique_ptr<ParserThread>                        parser_;
    std::array<std::unique_ptr<ShardWorker>, NUM_SHARDS> workers_;
    std::unique_ptr<RecoveryThread>                      recovery_;

    std::atomic<bool> running_{false};

    bool allocate_memory() noexcept;
    void init_workers();
    void start_workers();
    void stop_workers();
};

} // namespace itch
