#include "feed_handler.hpp"
#include "../../include/common/logging.hpp"
#include <cstring>
#include <stdexcept>

// toml++ for config parsing
#include <toml++/toml.hpp>

namespace itch {

// -----------------------------------------------------------------
// Config loading
// -----------------------------------------------------------------
FeedHandlerConfig load_config(const std::string& path) {
    FeedHandlerConfig cfg;
    try {
        auto tbl = toml::parse_file(path);

        if (auto net = tbl["network"].as_table()) {
            if (auto v = (*net)["multicast_group"].value<std::string>())
                cfg.multicast_group = *v;
            if (auto v = (*net)["port"].value<int64_t>())
                cfg.port = static_cast<uint16_t>(*v);
            if (auto v = (*net)["interface"].value<std::string>())
                cfg.interface = *v;
            if (auto v = (*net)["multicast_group_secondary"].value<std::string>())
                cfg.multicast_group_secondary = *v;
            if (auto v = (*net)["port_secondary"].value<int64_t>())
                cfg.port_secondary = static_cast<uint16_t>(*v);
            if (auto v = (*net)["interface_secondary"].value<std::string>())
                cfg.interface_secondary = *v;
            if (auto v = (*net)["failover_timeout_ms"].value<int64_t>())
                cfg.failover_timeout_ms = static_cast<uint64_t>(*v);
        }

        if (auto threads = tbl["threads"].as_table()) {
            if (auto v = (*threads)["receiver_core"].value<int64_t>())
                cfg.receiver_core = static_cast<int>(*v);
            if (auto v = (*threads)["parser_core"].value<int64_t>())
                cfg.parser_core = static_cast<int>(*v);
            if (auto v = (*threads)["recovery_core"].value<int64_t>())
                cfg.recovery_core = static_cast<int>(*v);
            if (auto v = (*threads)["merger_core"].value<int64_t>())
                cfg.merger_core = static_cast<int>(*v);
            if (auto arr = (*threads)["shard_cores"].as_array()) {
                for (std::size_t i = 0; i < NUM_SHARDS && i < arr->size(); ++i) {
                    if (auto c = arr->at(i).value<int64_t>())
                        cfg.shard_cores[i] = static_cast<int>(*c);
                }
            }
        }

        if (auto mon = tbl["monitoring"].as_table()) {
            if (auto v = (*mon)["report_interval_sec"].value<int64_t>())
                cfg.report_interval_sec = static_cast<uint32_t>(*v);
        }

        if (auto rec = tbl["recovery"].as_table()) {
            if (auto v = (*rec)["gap_timeout_ms"].value<int64_t>())
                cfg.gap_timeout_ms = static_cast<uint64_t>(*v);
            if (auto v = (*rec)["snapshot_interval_sec"].value<int64_t>())
                cfg.snapshot_interval_sec = static_cast<uint32_t>(*v);
            if (auto v = (*rec)["retransmit_address"].value<std::string>())
                cfg.retransmit_address = *v;
            if (auto v = (*rec)["retransmit_port"].value<int64_t>())
                cfg.retransmit_port = static_cast<uint16_t>(*v);
            if (auto v = (*rec)["retransmit_address_secondary"].value<std::string>())
                cfg.retransmit_address_secondary = *v;
            if (auto v = (*rec)["retransmit_port_secondary"].value<int64_t>())
                cfg.retransmit_port_secondary = static_cast<uint16_t>(*v);
            if (auto v = (*rec)["retransmit_username"].value<std::string>())
                cfg.retransmit_username = *v;
            if (auto v = (*rec)["retransmit_password"].value<std::string>())
                cfg.retransmit_password = *v;
            if (auto v = (*rec)["require_login"].value<bool>())
                cfg.require_login = *v;
            if (auto v = (*rec)["snapshot_directory"].value<std::string>())
                cfg.snapshot_dir = *v;
            if (auto v = (*rec)["snapshot_filename_template"].value<std::string>())
                cfg.snapshot_filename_template = *v;
        }

        if (auto ses = tbl["session"].as_table()) {
            if (auto v = (*ses)["initial_session"].value<std::string>())
                cfg.initial_session = *v;
            if (auto v = (*ses)["eod_archive_enabled"].value<bool>())
                cfg.eod_archive_enabled = *v;
            if (auto arr = (*ses)["watched_symbols"].as_array()) {
                for (auto& el : *arr) {
                    if (auto v = el.value<std::string>())
                        cfg.watched_symbols.push_back(*v);
                }
            }
        }

        if (auto per = tbl["persistence"].as_table()) {
            if (auto v = (*per)["last_sequence_dir"].value<std::string>())
                cfg.last_sequence_dir = *v;
        }

        if (auto shard = tbl["sharding"].as_table()) {
            if (auto v = (*shard)["queue_full_alert_threshold"].value<double>())
                cfg.queue_full_alert_threshold = *v;
        }

        ITCH_LOG_INFO("Config loaded from %s", path.c_str());
    } catch (const toml::parse_error& e) {
        ITCH_LOG_ERROR("Config parse error: %s", e.what());
        throw;
    }
    return cfg;
}

// -----------------------------------------------------------------
// FeedHandler
// -----------------------------------------------------------------
FeedHandler::FeedHandler(const std::string& config_path)
    : cfg_(load_config(config_path)),
      gap_detector_(cfg_.gap_timeout_ms) {}

FeedHandler::~FeedHandler() {
    stop();
    // Free huge-page memory
    if (ring_buf_)
        free_packet_ring_buffer(ring_buf_);
    if (secondary_ring_buf_)
        free_packet_ring_buffer(secondary_ring_buf_);
    for (auto& sm : shard_mem_) {
        if (sm.order_pool_mem)
            CpuAffinity::free_numa(sm.order_pool_mem,
                                   MAX_ORDERS_PER_SHARD * (sizeof(Order) + sizeof(uint32_t)));
        if (sm.order_map_mem)
            CpuAffinity::free_numa(sm.order_map_mem,
                                   ORDER_MAP_CAPACITY * sizeof(MapEntry));
    }
}

bool FeedHandler::allocate_memory() noexcept {
    // 1. Primary zero-copy packet ring buffer (huge pages)
    ring_buf_ = alloc_packet_ring_buffer();
    if (!ring_buf_) {
        ITCH_LOG_ERROR("Failed to allocate primary PacketRingBuffer");
        return false;
    }
    ring_.buffer = ring_buf_;

    // 2. Secondary ring buffer (only if B-feed is configured)
    if (!cfg_.multicast_group_secondary.empty()) {
        secondary_ring_buf_ = alloc_packet_ring_buffer();
        if (!secondary_ring_buf_) {
            ITCH_LOG_ERROR("Failed to allocate secondary PacketRingBuffer");
            return false;
        }
        secondary_ring_.buffer = secondary_ring_buf_;
        ITCH_LOG_INFO("Secondary ring buffer allocated (B-feed enabled)");
    }

    // 2. Per-shard OrderPool + OrderMap (NUMA-local where possible)
    for (ShardId s = 0; s < NUM_SHARDS; ++s) {
        int numa_node = CpuAffinity::numa_node_of_cpu(cfg_.shard_cores[s]);

        // Pool: capacity * (sizeof(Order) + sizeof(uint32_t) for freelist)
        std::size_t pool_bytes = MAX_ORDERS_PER_SHARD *
                                  (sizeof(Order) + sizeof(uint32_t));
        shard_mem_[s].order_pool_mem = CpuAffinity::alloc_on_numa(pool_bytes, numa_node);
        if (!shard_mem_[s].order_pool_mem) {
            ITCH_LOG_ERROR("Failed to allocate OrderPool memory for shard %u", s);
            return false;
        }
        shard_mem_[s].pool = std::make_unique<OrderPool>(
            MAX_ORDERS_PER_SHARD, shard_mem_[s].order_pool_mem);

        // Map: ORDER_MAP_CAPACITY * sizeof(MapEntry)
        std::size_t map_bytes = ORDER_MAP_CAPACITY * sizeof(MapEntry);
        shard_mem_[s].order_map_mem = CpuAffinity::alloc_on_numa(map_bytes, numa_node);
        if (!shard_mem_[s].order_map_mem) {
            ITCH_LOG_ERROR("Failed to allocate OrderMap memory for shard %u", s);
            return false;
        }
        std::memset(shard_mem_[s].order_map_mem, 0, map_bytes);
        shard_mem_[s].map = std::make_unique<OrderMap>(
            static_cast<MapEntry*>(shard_mem_[s].order_map_mem),
            ORDER_MAP_CAPACITY);

        // Book pool: BOOKS_PER_SHARD Level2Book objects, placement-new'd on NUMA.
        shard_mem_[s].book_pool = std::make_unique<OrderBookPool>(
            s, *shard_mem_[s].pool, *shard_mem_[s].map, numa_node);
        if (!shard_mem_[s].book_pool->ok()) {
            ITCH_LOG_ERROR("Failed to allocate OrderBookPool for shard %u", s);
            return false;
        }
    }

    ITCH_LOG_INFO("Memory allocated successfully");
    return true;
}

void FeedHandler::init_workers() {
    sym_dir_  = std::make_unique<SymbolDirectory>();
    bbo_pub_  = std::make_unique<BboPublisher>();
    {
        std::size_t pressure_threshold = static_cast<std::size_t>(
            SHARD_QUEUE_DEPTH * cfg_.queue_full_alert_threshold);
        router_ = std::make_unique<ShardRouter>(*sym_dir_, stats_, pressure_threshold);
    }

    // SessionManager — wires seq_tracker_, sym_dir_, bbo_pub_, router_ together.
    session_mgr_ = std::make_unique<SessionManager>(
        cfg_.initial_session.c_str(),
        seq_tracker_,
        *sym_dir_,
        *bbo_pub_,
        *router_,
        cfg_.eod_archive_enabled);

    // Per-shard EOD snapshot writers.
    // Each shard gets its own file: template with "_sN" inserted before the
    // last '.' so files are "itch_{session}_{ts}_s0.snap", etc.
    if (cfg_.eod_archive_enabled) {
        for (ShardId s = 0; s < NUM_SHARDS; ++s) {
            // Derive shard-specific filename template
            std::string t = cfg_.snapshot_filename_template;
            std::string suffix = "_s" + std::to_string(s);
            auto dot = t.rfind('.');
            if (dot != std::string::npos)
                t.insert(dot, suffix);
            else
                t += suffix;
            eod_snapshots_[s] = std::make_unique<BookSnapshot>(cfg_.snapshot_dir, t);

            // Register EOD callback: save this shard's books on 'Q'/'M'
            OrderBookPool* pool = shard_mem_[s].book_pool.get();
            BookSnapshot*  snap = eod_snapshots_[s].get();
            session_mgr_->register_eod_callback(
                [pool, snap](const Level2Book* /*books*/, std::size_t /*count*/,
                             const char* session_10) noexcept {
                    snap->save_eod(pool->slab(), OrderBookPool::BOOKS_PER_SHARD,
                                   session_10);
                });
        }
    }

    // Register watched tickers for pre-open warmup book activation.
    // Each entry in cfg_.watched_symbols is an 8-char space-padded ITCH ticker.
    for (const auto& sym : cfg_.watched_symbols) {
        char ticker8[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
        std::size_t n = std::min(sym.size(), std::size_t{8});
        std::memcpy(ticker8, sym.c_str(), n);
        session_mgr_->add_watched_ticker(ticker8);
    }

    // Metrics reporter
    LatencyHistogram* book_lat_arr = &shard_mem_[0].book_lat;
    metrics_ = std::make_unique<MetricsReporter>(
        cfg_.report_interval_sec, stats_, &parse_lat_, book_lat_arr, NUM_SHARDS);

    // Primary receiver
    ThreadConfig recv_cfg{cfg_.receiver_core, 1, 80, -1};
    receiver_ = std::make_unique<ReceiverThread>(ring_, stats_, recv_cfg);

    // Secondary receiver (B-feed) — only when multicast_group_secondary is set
    if (!cfg_.multicast_group_secondary.empty()) {
        // Secondary receiver runs on core merger_core+1; SCHED_FIFO 80 (same as primary).
        // Using a separate core keeps both receivers spinning independently.
        ThreadConfig recv2_cfg{cfg_.merger_core + 1, 1, 80, -1};
        secondary_receiver_ = std::make_unique<ReceiverThread>(
            secondary_ring_, stats_, recv2_cfg);
        ITCH_LOG_INFO("Secondary receiver created (group=%s port=%u iface=%s)",
                      cfg_.multicast_group_secondary.c_str(),
                      cfg_.port_secondary,
                      cfg_.interface_secondary.c_str());
    }

    // FeedMerger — merges primary + secondary, deduplicates, forwards to merged_queue_
    ThreadConfig merger_cfg{cfg_.merger_core, 1, 77, -1};
    feed_merger_ = std::make_unique<FeedMerger>(
        ring_, secondary_ring_, merged_queue_, stats_,
        cfg_.failover_timeout_ms, merger_cfg);

    // Sequence persistence writer (started in start_workers, after load())
    persist_seq_ = std::make_unique<PersistentSequence>(cfg_.last_sequence_dir);

    // Parser — now reads from merged_queue_ instead of directly from ring_
    ThreadConfig parser_cfg{cfg_.parser_core, 1, 79, -1};
    parser_ = std::make_unique<ParserThread>(ring_, secondary_ring_,
                                              merged_queue_,
                                              *router_, parse_lat_,
                                              stats_, gap_detector_,
                                              seq_tracker_, *session_mgr_,
                                              *persist_seq_,
                                              parser_cfg);

    // Shard workers
    for (ShardId s = 0; s < NUM_SHARDS; ++s) {
        ThreadConfig shard_cfg{cfg_.shard_cores[s], 1, 78, -1};
        workers_[s] = std::make_unique<ShardWorker>(
            s, router_->queue(s), *bbo_pub_,
            shard_mem_[s].book_lat, *sym_dir_,
            *shard_mem_[s].pool, *shard_mem_[s].map,
            *shard_mem_[s].book_pool,
            *session_mgr_,
            stats_,
            shard_cfg);
    }

    // Recovery
    ThreadConfig recovery_cfg{cfg_.recovery_core, 0, 0, -1};
    RetransmitCredentials creds{cfg_.retransmit_username,
                                cfg_.retransmit_password,
                                cfg_.require_login};
    recovery_ = std::make_unique<RecoveryThread>(
        gap_detector_, seq_tracker_, ring_,
        cfg_.retransmit_address, cfg_.retransmit_port,
        cfg_.retransmit_address_secondary, cfg_.retransmit_port_secondary,
        creds,
        cfg_.snapshot_dir, cfg_.snapshot_filename_template,
        cfg_.snapshot_interval_sec,
        session_mgr_->current_session(),
        stats_,
        recovery_cfg);
}

bool FeedHandler::init() noexcept {
    ITCH_LOG_INFO("FeedHandler initialising...");

    // Reject the RFC 5737 documentation placeholder before any network I/O.
    // 198.51.100.1 (TEST-NET-2) is permanently non-routable; if it reaches
    // production the process would silently never recover gaps.
    if (cfg_.retransmit_address == "198.51.100.1") {
        fprintf(stderr,
                "[FATAL] retransmit_address is the RFC 5737 documentation placeholder "
                "\"198.51.100.1\". Set a real recovery server address in "
                "config [recovery].retransmit_address and restart.\n");
        exit(1);
    }

    if (!allocate_memory())
        return false;

    try {
        init_workers();
    } catch (const std::exception& e) {
        ITCH_LOG_ERROR("init_workers failed: %s", e.what());
        return false;
    }

    // Seed SequenceTracker from the last persisted position so the process
    // resumes without declaring a false gap on the first in-order packet.
    {
        auto loaded = persist_seq_->load();
        if (loaded.seq > 0) {
            seq_tracker_.reset(loaded.seq);
            ITCH_LOG_INFO("FeedHandler: warm start — resuming from seq=%lu session='%.10s'",
                          loaded.seq, loaded.session);
        }
    }

    if (!receiver_->open(cfg_.interface, cfg_.port,
                          cfg_.multicast_group, cfg_.interface)) {
        ITCH_LOG_ERROR("Failed to open primary receiver socket");
        return false;
    }

    if (secondary_receiver_) {
        uint16_t sec_port = cfg_.port_secondary ? cfg_.port_secondary : cfg_.port;
        const std::string& sec_iface = cfg_.interface_secondary.empty()
            ? cfg_.interface : cfg_.interface_secondary;
        if (!secondary_receiver_->open(sec_iface, sec_port,
                                       cfg_.multicast_group_secondary, sec_iface)) {
            ITCH_LOG_ERROR("Failed to open secondary receiver socket — B-feed disabled");
            secondary_receiver_.reset(); // disable gracefully; don't abort
        }
    }

    ITCH_LOG_INFO("FeedHandler init complete. Primary: %s:%u mcast=%s  Secondary: %s",
                  cfg_.interface.c_str(), cfg_.port, cfg_.multicast_group.c_str(),
                  cfg_.multicast_group_secondary.empty() ? "disabled"
                      : cfg_.multicast_group_secondary.c_str());
    return true;
}

void FeedHandler::run() {
    running_.store(true, std::memory_order_release);
    start_workers();
    // Block until stop() is called (e.g., from signal handler)
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop_workers();
}

void FeedHandler::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        stop_workers();
    }
}

void FeedHandler::start_workers() {
    // Start in reverse order of data flow (consumers first, then producers)
    metrics_->start();
    recovery_->start();
    persist_seq_->start(); // ready before parser begins enqueuing
    for (ShardId s = 0; s < NUM_SHARDS; ++s)
        workers_[s]->start();
    parser_->start();
    feed_merger_->start();
    if (secondary_receiver_) secondary_receiver_->start();
    receiver_->start();
    ITCH_LOG_INFO("All workers started");
}

void FeedHandler::stop_workers() {
    // Stop in order of data flow (producers first, then consumers)
    if (receiver_)           receiver_->stop();
    if (secondary_receiver_) secondary_receiver_->stop();
    if (feed_merger_)        feed_merger_->stop();
    if (parser_)             parser_->stop();
    // Flush persist queue after parser stops — no more enqueues at this point.
    if (persist_seq_) persist_seq_->stop();
    for (ShardId s = 0; s < NUM_SHARDS; ++s)
        if (workers_[s]) workers_[s]->stop();
    if (recovery_)    recovery_->stop();
    if (metrics_)     metrics_->stop();
    ITCH_LOG_INFO("All workers stopped");
}

} // namespace itch
