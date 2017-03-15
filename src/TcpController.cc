#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "TcpController.h"

namespace net_stack {

namespace {
uint32 kThreadPoolSize = 6;
}

TcpController::TcpController(Host* host, const TcpControllerOptions& options) :
    host_(host),
    thread_pool_(kThreadPoolSize),
    key_(options.key),
    send_buffer_(options.send_buffer_size),
    send_window_(options.send_window_base, options.send_window_size),
    recv_buffer_(options.recv_buffer_size),
    recv_window_(options.recv_window_base, options.recv_window_size) {
}

// This method just enqueue the new packet into this TCP connection's private
// packet receive buffer. It is PacketReceiveBufferListner that monitors this
// queue and handles packets.
void TcpController::ReceiveNewPacket(std::unique_ptr<Packet> packet) {
  std::unique_lock<std::mutex> lock(pkt_recv_buffer_mutex_);
  pkt_recv_buffer_.Push(std::move(packet));
}

void TcpController::PacketReceiveBufferListner() {
  while (1) {
    std::queue<std::unique_ptr<Packet>> new_packets;
    {
      std::unique_lock<std::mutex> lock(pkt_recv_buffer_mutex_);
      pkt_recv_buffer_cv_.wait(lock,
          [this] { return !pkt_recv_buffer_.empty(); });

      // Get all new packets.
      pkt_recv_buffer_.DeQueueAllTo(&new_packets);
    }

    HandleReceivedPackets(&new_packets);
    SANITY_CHECK(new_packets.empty(),
                 "New packets queue should have been cleared");
  }
}

void TcpController::HandleReceivedPackets(
    std::queue<std::unique_ptr<Packet>>* new_packets) {
  while (!new_packets.empty()) {
    std::unique_ptr<Packet> pkt = std::move(new_packets.front());
    new_packets.pop();

    if (pkt->tcp_header().ack) {
      // Handle ack packet.
    } else {
      // Handle data packet.
    }
  }
}

}  // namespace net_stack
