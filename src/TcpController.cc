#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "Base/Utils.h"
#include "TcpController.h"

namespace net_stack {

namespace {
uint32 kThreadPoolSize = 6;
uint32 kDefaultDataPacketSize = 1500;
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
  while (!new_packets->empty()) {
    std::unique_ptr<Packet> pkt = std::move(new_packets->front());
    new_packets->pop();

    if (pkt->tcp_header().ack) {
      // Handle ack packet. If detect duplicated ACKs, do a fast re-transmit.
      bool re_transmit = send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
      if (re_transmit) {
        SendPacket(send_window_.BasePakcketWaitingForAck());
      }
    } else {
      // Handle data packet. It sends a ack packet back to sender, and deliver
      // packets to upper layer (socket receive buffer) if avaible.
      auto pair = recv_window_.ReceivePacket(pkt);
      SendPacket(MakeAckPacket(pair.first));
      StreamDataToReceiveBuffer(pair.second);
    }
  }
}

std::unique_ptr<Packet> TcpController::MakeAckPacket(uint32 ack_num) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = true;
  tcp_header.ack_num = ack_num;
  std::unique_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  return pkt;
}

void TcpController::StreamDataToReceiveBuffer(
    std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes) {
  {
    std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
    std::shared_ptr<RecvWindowNode> node = received_pkt_nodes;
    while (node) {
      recv_buffer_.Write(node->pkt->paylaod(), node->pkt->paylaod_size());
      node = node->next;
    }
  }
  recv_buffer_cv_.notify_one();
}

// TODO: add support for non-blocking read.
int32 TcpController::ReadData(byte* buf, int32 size) {
  std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
  pkt_recv_buffer_cv_.wait(lock,
      [this] { return !recv_buffer_.empty(); });

  // Copy data to user buffer.
  uint32 readn = recv_buffer_.Read(buf, size);

  // Cast should be safe. We'll never have a receive buffer as big as 2^31
  return static_cast<int32>readn;
}

void TcpController::SocketSendBufferListener() {
  std::unique_lock<std::mutex> lock(send_buffer_mutex_);
  send_buffer_cv_.wait(lock,
      [this] { return !send_buffer_.empty(); });

  // Create data packets and send them out.
  uint32 size_to_send =
      Utils::Min(send_window_.free_space(), send_buffer_.size());
  uint32 num_pkts = size_to_send / kDefaultDataPacketSize;
  uint32 last_pkt_size = size_to_send % kDefaultDataPacketSize;
  for (uint32 i = 0; i < num_pkts; i++) {
    auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                       send_buffer_, kDefaultDataPacketSize);
    // This just mark the new pkt into send window.
    send_window_.SendPacket(new_data_pkt);
    // Really send the packet.
    SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
  }

  if (last_pkt_size > 0) {
    auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                       send_buffer_, last_pkt_size);
    send_window_.SendPacket(new_data_pkt);
    SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
  }
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, const char* data, uint32 size) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = false;
  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header, data, size));
  return pkt;
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, BufferInterface* data_buffer, uint32 size) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = false;
  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  pkt.InjectPayloadFromBuffer(data_buffer, size);
  return pkt;
}

}  // namespace net_stack
