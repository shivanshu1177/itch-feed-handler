// pcap_replay — replay a PCAP file of ITCH UDP traffic.
// Usage: pcap_replay <file.pcap> [--speed N] [--stats-interval S]
//
// --speed 0   : replay as fast as possible (throughput test)
// --speed 1.0 : replay at original timing (1x)
// --speed 2.0 : replay at 2x speed
//
// Requires libpcap.

#include "protocol/itch_messages.hpp"
#include "protocol/itch_parser.hpp"
#include "protocol/moldudp64_parser.hpp"
#include "protocol/sequence_tracker.hpp"
#include "common/timestamp.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <pcap.h>

using namespace itch;

struct ReplayStats {
    uint64_t packets     = 0;
    uint64_t messages    = 0;
    uint64_t gaps        = 0;
    uint64_t heartbeats  = 0;

    // Per-type counters
    uint64_t add_orders     = 0;
    uint64_t executions     = 0;
    uint64_t cancels        = 0;
    uint64_t deletes        = 0;
    uint64_t replaces       = 0;
};

static void print_stats(const ReplayStats& s, double elapsed_sec) {
    printf("\n===== Replay Statistics =====\n");
    printf("  Packets:       %lu  (%.0f pkt/s)\n", s.packets,
           elapsed_sec > 0 ? s.packets / elapsed_sec : 0.0);
    printf("  Messages:      %lu  (%.0f msg/s)\n", s.messages,
           elapsed_sec > 0 ? s.messages / elapsed_sec : 0.0);
    printf("  Heartbeats:    %lu\n", s.heartbeats);
    printf("  Sequence gaps: %lu\n", s.gaps);
    printf("  Add orders:    %lu\n", s.add_orders);
    printf("  Executions:    %lu\n", s.executions);
    printf("  Cancels:       %lu\n", s.cancels);
    printf("  Deletes:       %lu\n", s.deletes);
    printf("  Replaces:      %lu\n", s.replaces);
    printf("=============================\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.pcap> [--speed N] [--stats-interval S]\n", argv[0]);
        return 1;
    }

    const char* pcap_path = argv[1];
    double speed = 0.0; // 0 = max speed
    int stats_interval_sec = 10;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc)
            speed = atof(argv[++i]);
        else if (strcmp(argv[i], "--stats-interval") == 0 && i + 1 < argc)
            stats_interval_sec = atoi(argv[++i]);
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* pcap = pcap_open_offline(pcap_path, errbuf);
    if (!pcap) {
        fprintf(stderr, "Cannot open %s: %s\n", pcap_path, errbuf);
        return 1;
    }

    printf("Replaying %s at %s speed\n",
           pcap_path, speed == 0.0 ? "MAX" : "1x");

    SequenceTracker seq_tracker(0);
    ReplayStats     stats;
    ItchMsg         buf[ItchParser::MAX_MSGS_PER_PACKET];

    struct pcap_pkthdr* hdr;
    const uint8_t*      raw;
    bool                first_pkt = true;
    struct timeval      first_ts{};
    Timestamp           wall_start = steady_ns();
    Timestamp           last_stats_print = wall_start;

    while (pcap_next_ex(pcap, &hdr, &raw) == 1) {
        // Timing: delay to match original capture timestamps (if speed > 0)
        if (speed > 0.0) {
            if (first_pkt) {
                first_ts    = hdr->ts;
                first_pkt   = false;
            }
            double pkt_offset_us = (hdr->ts.tv_sec - first_ts.tv_sec) * 1e6 +
                                   (hdr->ts.tv_usec - first_ts.tv_usec);
            double target_us = pkt_offset_us / speed;
            Timestamp elapsed_ns = steady_ns() - wall_start;
            double elapsed_us = elapsed_ns / 1000.0;
            if (target_us > elapsed_us + 10.0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(
                        static_cast<long>(target_us - elapsed_us)));
            }
        }

        const int UDP_PAYLOAD_OFFSET = 42;
        if (hdr->caplen <= static_cast<uint32_t>(UDP_PAYLOAD_OFFSET))
            continue;

        const uint8_t* udp = raw + UDP_PAYLOAD_OFFSET;
        uint16_t len = static_cast<uint16_t>(hdr->caplen - UDP_PAYLOAD_OFFSET);

        auto mold = MoldUDP64Parser::parse(udp, len);
        if (!mold.valid)
            continue;

        if (mold.header.is_heartbeat()) {
            ++stats.heartbeats;
            continue;
        }

        ++stats.packets;

        // Sequence tracking
        bool ok = seq_tracker.try_advance(mold.header.get_sequence());
        if (!ok && seq_tracker.gap_count() > stats.gaps)
            ++stats.gaps;

        int count = ItchParser::parse(udp, len, 0, mold, buf, ItchParser::MAX_MSGS_PER_PACKET);
        stats.messages += static_cast<uint64_t>(count);

        for (int i = 0; i < count; ++i) {
            switch (buf[i].type) {
            case MessageType::AddOrder:
            case MessageType::AddOrderMPID:  ++stats.add_orders; break;
            case MessageType::OrderExecuted:
            case MessageType::OrderExecutedPrice: ++stats.executions; break;
            case MessageType::OrderCancel:   ++stats.cancels; break;
            case MessageType::OrderDelete:   ++stats.deletes; break;
            case MessageType::OrderReplace:  ++stats.replaces; break;
            default: break;
            }
        }

        // Periodic stats print
        Timestamp now = steady_ns();
        if (stats_interval_sec > 0 &&
            (now - last_stats_print) > static_cast<uint64_t>(stats_interval_sec) * 1'000'000'000ULL) {
            double elapsed = (now - wall_start) / 1e9;
            printf("[%6.1fs] pkts=%-10lu msgs=%-12lu gaps=%lu\n",
                   elapsed, stats.packets, stats.messages, stats.gaps);
            last_stats_print = now;
        }
    }

    pcap_close(pcap);

    double elapsed = (steady_ns() - wall_start) / 1e9;
    print_stats(stats, elapsed);
    return 0;
}
