#include "receiver_thread.hpp"
#include "../../include/common/logging.hpp"

namespace itch {

bool ReceiverThread::open(const std::string& addr, uint16_t port,
                           const std::string& mcast_group,
                           const std::string& mcast_iface) noexcept {
    if (!sock_.open(addr, port))
        return false;
    if (!mcast_group.empty() && !sock_.join_multicast(mcast_group, mcast_iface))
        return false;
    return true;
}

void ReceiverThread::start() {
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ReceiverThread::run, this);
    ITCH_LOG_INFO("ReceiverThread started (cpu=%d)", cfg_.cpu_id);
}

void ReceiverThread::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
    sock_.close();
}

void ReceiverThread::run() noexcept {
    CpuAffinity::apply(cfg_);

    while (running_.load(std::memory_order_acquire)) {
        // Claim the next write slot — no allocation, direct pointer into ring buffer
        PacketSlot* slot = ring_.claim(write_slot_);

        int n = sock_.recv(*reinterpret_cast<RawPacket*>(slot)); // reuse recv interface
        // Note: SocketReceiver::recv() writes into pkt.data, length, recv_timestamp_ns.
        // Since PacketSlot has the same layout prefix as RawPacket, this is safe.
        // (See static_assert in packet_ring.hpp / packet.hpp if layout ever diverges.)

        if (n > 0) {
            slot->length   = static_cast<uint16_t>(n);
            // recv_ts already set by SocketReceiver
            if (!ring_.publish(write_slot_)) {
                // Parser is too slow — ring full, drop packet
                ++pkts_dropped_;
                stats_.packets_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++pkts_received_;
                stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
                ++write_slot_; // advance monotonic slot counter (wraps via & mask in claim)
            }
        }
        // n == 0: EAGAIN, spin again
        // n <  0: error already logged by SocketReceiver
    }
}

} // namespace itch
