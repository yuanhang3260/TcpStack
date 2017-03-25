#include <chrono>

#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "Base/Utils.h"
#include "TcpController.h"

namespace net_stack {

namespace {
uint32 kThreadPoolSize = 6;
uint32 kDefaultDataPacketSize = 1000;
}

TcpController::TcpController(Host* host, const TcpControllerOptions& options) :
    host_(host),
    thread_pool_(kThreadPoolSize),
    key_(options.key),
    recv_window_(options.recv_window_base, options.recv_window_size),
    recv_buffer_(options.recv_buffer_size),
    send_buffer_(options.send_buffer_size),
    send_window_(options.send_window_base, options.send_window_size),
    timer_(std::chrono::seconds(1),
           std::bind(&TcpController::TimeoutReTransmitter, this)) {
}

// This method just enqueue the new packet into this TCP connection's private
// packet receive buffer. It is PacketReceiveBufferListner that monitors this
// queue and handles packets.
void TcpController::ReceiveNewPacket(std::unique_ptr<Packet> packet) {
  {
    std::unique_lock<std::mutex> lock(pkt_recv_buffer_mutex_);
    pkt_recv_buffer_.Push(std::move(packet));
  }
  pkt_recv_buffer_cv_.notify_one();
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
      std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);

      // Handle ack packet. If detect duplicated ACKs, do a fast re-transmit.
      bool re_transmit = send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
      if (re_transmit) {
        SendPacket(send_window_.BasePakcketWaitingForAck());
      }
      // If send window is cleared, stop the timer.
      if (send_window_.NumPacketsToAck() == 0) {
        timer_.Stop();
      }

      // If send window has free space, notify packet send thread.
      if (send_window_.free_space() > 0) {
        send_window_cv_.notify_one();
      }
    } else {
      // Handle data packet. It sends a ack packet back to sender, and deliver
      // packets to upper layer (socket receive buffer) if avaible.
      auto pair = recv_window_.ReceivePacket(std::move(pkt));
      SendPacket(std::move(MakeAckPacket(pair.first)));
      StreamDataToReceiveBuffer(pair.second);
    }
  }
}

void TcpController::StreamDataToReceiveBuffer(
    std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes) {
  {
    std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
    std::shared_ptr<RecvWindow::RecvWindowNode> node = received_pkt_nodes;
    while (node) {
      uint32 writen =
          recv_buffer_.Write(node->pkt->payload(), node->pkt->payload_size());
      if (writen <= 0) {
        LogERROR("Socket receive buffer is full, pkt seq = %u is dropped.",
                 node->pkt->tcp_header().seq_num);
      }
      node = node->next;
    }
  }
  recv_buffer_cv_.notify_one();
}

// TODO: add support for non-blocking read.
int32 TcpController::ReadData(byte* buf, int32 size) {
  std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
  recv_buffer_cv_.wait(lock,
      [this] { return !recv_buffer_.empty(); });

  // Copy data to user buffer.
  // TODO: replace with RingBuffer to check flow control.
  uint32 readn = recv_buffer_.Read(buf, size);

  // Cast should be safe. We'll never have a receive buffer as big as 2^31
  return static_cast<int32>(readn);
}

int32 TcpController::WriteData(const byte* buf, int32 size) {
  uint32 writen = 0;
  {
    std::unique_lock<std::mutex> lock(send_buffer_mutex_);
    // Non-blocking mode?
    send_buffer_write_cv_.wait(lock,
        [this] { return !send_buffer_.full(); });
    writen = send_buffer_.Write(buf, size);
  }

  send_buffer_cv_.notify_one();
  return static_cast<int32>(writen);
}

void TcpController::SocketSendBufferListener() {
  while (true) {
    // Wait for send window to be not full.
    std::unique_lock<std::mutex> lock_send_window(send_window_mutex_);
    send_window_cv_.wait(lock_send_window,
        [this] { return !send_window_.free_space() > 0; });
    lock_send_window.unlock();

    // Wait for socket send buffer to have data to send.
    std::unique_lock<std::mutex> lock_send_buffer(send_buffer_mutex_);
    send_buffer_cv_.wait(lock_send_buffer,
        [this] { return !send_buffer_.empty(); });

    // Lock send window again. We're sure it has free space now, because it
    // has passed the previous conditional wait.
    lock_send_window.lock();
    if (send_window_.free_space() <= 0) {
      LogERROR("This should NOT happen. Send window must have space");
      continue;
    }

    // Create data packets and send them out.
    uint32 size_to_send =
        Utils::Min(send_window_.free_space(), send_buffer_.size());
    uint32 num_pkts = size_to_send / kDefaultDataPacketSize;
    uint32 last_pkt_size = size_to_send % kDefaultDataPacketSize;
    for (uint32 i = 0; i < num_pkts; i++) {
      bool restart_timer = (send_window_.NumPacketsToAck() == 0);

      auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                         &send_buffer_, kDefaultDataPacketSize);
      // This just mark the new pkt into send window.
      send_window_.SendPacket(new_data_pkt);
      // Really send the packet.
      SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
      if (restart_timer) {
        timer_.Restart();
      }
    }

    if (last_pkt_size > 0) {
      bool restart_timer = (send_window_.size() == 0);
      auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                         &send_buffer_, last_pkt_size);
      send_window_.SendPacket(new_data_pkt);
      SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
      if (restart_timer) {
        timer_.Restart();
      }
    }
    // Notify new data can be written to send buffer from user space.
    send_buffer_write_cv_.notify_one();
  }
}

void TcpController::SendPacket(std::unique_ptr<Packet> pkt) {
  {
    std::unique_lock<std::mutex> lock(pkt_send_buffer_mutex_);
    pkt_send_buffer_.Push(std::move(pkt));
  }
  pkt_send_buffer_cv_.notify_one();
}

void TcpController::PacketSendBufferListner() {
  while (true) {
    std::unique_lock<std::mutex> lock(pkt_send_buffer_mutex_);
    pkt_send_buffer_cv_.wait(lock,
        [this] { return !pkt_send_buffer_.empty(); });

    // Deliver packets to host buffer.
    //host->GetPacketsFromLocalTcp(pkt_send_buffer_);
  }
}

void TcpController::TimeoutReTransmitter() {
  // Current timer expired. Re-transmit the oldest packet in send window. Note
  // timer will be automatically restarted.
  std::unique_lock<std::mutex> lock(send_window_mutex_);
  if (send_window_.NumPacketsToAck() > 0) {
    SendPacket(send_window_.BasePakcketWaitingForAck());
  }
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, const byte* data, uint32 size) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = false;
  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header, data, size));
  return pkt;
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, Utility::BufferInterface* data_buffer, uint32 size) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = false;
  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  pkt->InjectPayloadFromBuffer(data_buffer, size);
  return pkt;
}

std::unique_ptr<Packet> TcpController::MakeAckPacket(uint32 ack_num) {
  IPHeader ip_header;
  TcpHeader tcp_header;
  tcp_header.ack = true;
  tcp_header.ack_num = ack_num;
  std::unique_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  return pkt;
}

}  // namespace net_stack
