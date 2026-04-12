// itch_inspect — parse a PCAP file and dump human-readable ITCH messages.
// Usage: itch_inspect <file.pcap> [--max-packets N] [--locate CODE]
//
// Requires libpcap.

#include "protocol/itch_messages.hpp"
#include "protocol/itch_parser.hpp"
#include "protocol/moldudp64_parser.hpp"
#include "common/types.hpp"
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pcap.h>

using namespace itch;

static void print_msg(const ItchMsg& m) {
    // Common header
    printf("[%lu ns] locate=%-5u type=%c  ",
           m.timestamp, m.locate, static_cast<char>(m.type));

    switch (m.type) {
    case MessageType::AddOrder:
    case MessageType::AddOrderMPID:
        printf("ref=%-12lu side=%c shares=%-8u price=%8.4f  stock=%.8s",
               m.add_order.ref,
               static_cast<char>(m.add_order.side),
               m.add_order.shares,
               m.add_order.price / 10000.0,
               m.add_order.stock);
        break;
    case MessageType::OrderExecuted:
        printf("ref=%-12lu exec=%-8u  match=%lu",
               m.order_executed.ref,
               m.order_executed.executed_shares,
               m.order_executed.match_number);
        break;
    case MessageType::OrderExecutedPrice:
        printf("ref=%-12lu exec=%-8u  match=%lu  px=%8.4f",
               m.order_executed_price.ref,
               m.order_executed_price.executed_shares,
               m.order_executed_price.match_number,
               m.order_executed_price.price / 10000.0);
        break;
    case MessageType::OrderCancel:
        printf("ref=%-12lu cancelled=%-8u",
               m.order_cancel.ref, m.order_cancel.cancelled_shares);
        break;
    case MessageType::OrderDelete:
        printf("ref=%-12lu", m.order_delete.ref);
        break;
    case MessageType::OrderReplace:
        printf("orig=%-12lu  new=%-12lu  px=%8.4f  qty=%u",
               m.order_replace.orig_ref, m.order_replace.new_ref,
               m.order_replace.new_price / 10000.0,
               m.order_replace.new_shares);
        break;
    case MessageType::StockDirectory:
        printf("stock=%.8s  category=%c  lot=%u",
               m.stock_directory.stock,
               m.stock_directory.market_category,
               m.stock_directory.round_lot_size);
        break;
    case MessageType::Trade:
        printf("ref=%-12lu  side=%c  qty=%-8u  px=%8.4f  stock=%.8s  match=%lu",
               m.trade.ref, static_cast<char>(m.trade.side),
               m.trade.shares, m.trade.price / 10000.0,
               m.trade.stock, m.trade.match_number);
        break;
    case MessageType::SystemEvent:
        printf("event=%c", m.system_event.event_code);
        break;
    default:
        break;
    }
    printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.pcap> [--max N] [--locate CODE]\n", argv[0]);
        return 1;
    }

    const char* pcap_path = argv[1];
    int max_packets = INT32_MAX;
    LocateCode filter_locate = 0; // 0 = no filter

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            max_packets = atoi(argv[++i]);
        else if (strcmp(argv[i], "--locate") == 0 && i + 1 < argc)
            filter_locate = static_cast<LocateCode>(atoi(argv[++i]));
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* pcap = pcap_open_offline(pcap_path, errbuf);
    if (!pcap) {
        fprintf(stderr, "Cannot open %s: %s\n", pcap_path, errbuf);
        return 1;
    }

    printf("Reading %s\n", pcap_path);

    struct pcap_pkthdr* hdr;
    const uint8_t* raw;
    int pkt_count = 0;
    uint64_t msg_count = 0;

    ItchMsg buf[ItchParser::MAX_MSGS_PER_PACKET];

    while (pkt_count < max_packets && pcap_next_ex(pcap, &hdr, &raw) == 1) {
        ++pkt_count;

        // Skip Ethernet(14) + IP(20) + UDP(8) = 42 bytes header
        // (assumes standard Ethernet + IPv4 + UDP; adjust if VLAN tagged)
        const int UDP_PAYLOAD_OFFSET = 42;
        if (hdr->caplen <= static_cast<uint32_t>(UDP_PAYLOAD_OFFSET))
            continue;

        const uint8_t* udp_payload = raw + UDP_PAYLOAD_OFFSET;
        uint16_t       udp_len     = static_cast<uint16_t>(hdr->caplen - UDP_PAYLOAD_OFFSET);

        auto mold = MoldUDP64Parser::parse(udp_payload, udp_len);
        if (!mold.valid || mold.header.is_heartbeat())
            continue;

        int count = ItchParser::parse(udp_payload, udp_len, 0, mold,
                                       buf, ItchParser::MAX_MSGS_PER_PACKET);
        for (int i = 0; i < count; ++i) {
            if (filter_locate != 0 && buf[i].locate != filter_locate)
                continue;
            print_msg(buf[i]);
            ++msg_count;
        }
    }

    pcap_close(pcap);
    printf("\nProcessed %d packets, %lu messages\n", pkt_count, msg_count);
    return 0;
}
