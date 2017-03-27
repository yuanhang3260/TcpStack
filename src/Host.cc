#include "Base/Ptr.h"
#include "Base/Log.h"
#include "Host.h"

namespace net_stack {

Host::Host(const std::string& ip_address, BaseChannel* channel) :
    ip_address_(ip_address),
    channel_(channel),
    thread_pool_(2) {
  thread_pool_.AddTask(std::bind(&Host::PacketsReceiveListener, this));
  thread_pool_.AddTask(std::bind(&Host::PacketsSendListener, this));
  thread_pool_.Start();
}

void Host::MovePacketsFromChannel(
    std::queue<std::unique_ptr<Packet>>* packets_to_receive) {
  {
    std::unique_lock<std::mutex> lock(recv_pkt_queue_mutex_);
    recv_pkt_queue_.Push(packets_to_receive);
    SANITY_CHECK(packets_to_receive->empty(), "packets_to_receive not emptied");
  }
  recv_pkt_queue_cv_.notify_one();
}

void Host::PacketsReceiveListener() {
  while (true) {
    std::queue<std::unique_ptr<Packet>> new_packets;
    {
      std::unique_lock<std::mutex> lock(recv_pkt_queue_mutex_);
      recv_pkt_queue_cv_.wait(lock,
                              [this] { return !recv_pkt_queue_.empty(); });
      recv_pkt_queue_.DeQueueAllTo(&new_packets);
    }
    DemultiplexPacketsToTcps(&new_packets);
  }
}

void Host::DemultiplexPacketsToTcps(
    std::queue<std::unique_ptr<Packet>>* new_packets) {
  uint32 size = new_packets->size();
  for (uint32 i = 0; i < size; i++) {
    auto& pkt = new_packets->front();
    TcpControllerKey tcp_key{pkt->ip_header().source_ip,
                             pkt->tcp_header().source_port,
                             pkt->ip_header().dest_ip,
                             pkt->tcp_header().dest_port};
    auto it = connections_.find(tcp_key);
    if (it == connections_.end()) {
      LogERROR("Can't find tcp connection %s", tcp_key.DebugString().c_str());
      continue;
    }
    it->second->ReceiveNewPacket(std::move(pkt));
    new_packets->pop();
  }
}

void Host::MultiplexPacketsFromTcp(
    std::queue<std::unique_ptr<Packet>>* packets_to_send) {
  {
    std::unique_lock<std::mutex> lock(send_pkt_queue_mutex_);
    send_pkt_queue_.Push(packets_to_send);
    SANITY_CHECK(packets_to_send->empty(), "packets_to_send not emptied!");
  }
  send_pkt_queue_cv_.notify_one();
}

void Host::PacketsSendListener() {
  while (true) {
    std::queue<std::unique_ptr<Packet>> packets_to_send;
    {
      std::unique_lock<std::mutex> lock(send_pkt_queue_mutex_);
      send_pkt_queue_cv_.wait(lock,
                              [this] { return !send_pkt_queue_.empty(); });
      send_pkt_queue_.DeQueueAllTo(&packets_to_send);
    }
    // Send to channel.
    channel_->Send(&packets_to_send);
  }
}

void Host::CreateTcpConnection(const std::string& source_ip, uint32 source_port,
                               uint32 local_port, uint32 socket_fd) {
  TcpControllerKey tcp_key{source_ip, source_port, ip_address_, local_port};
  connections_.emplace(tcp_key,
                       ptr::MakeUnique<TcpController>(
                          this, tcp_key, socket_fd,
                          TcpController::GetDefaultOptions()));

  std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
  socket_tcp_map_.emplace(socket_fd, connections_.at(tcp_key).get());
}

int32 Host::ReadData(uint32 socket_fd, byte* buffer, int32 size) {
  TcpController* tcp_conn;
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    auto it = socket_tcp_map_.find(socket_fd);
    if (it == socket_tcp_map_.end()) {
      LogERROR("Can't find tcp connection bind with socket %u", socket_fd);
    }
    tcp_conn = it->second;
  }
  return tcp_conn->ReadData(buffer, size);
}

int32 Host::WriteData(uint32 socket_fd, const byte* buffer, int32 size) {
  TcpController* tcp_conn;
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    auto it = socket_tcp_map_.find(socket_fd);
    if (it == socket_tcp_map_.end()) {
      LogERROR("Can't find tcp connection bind with socket %u", socket_fd);
    }
    tcp_conn = it->second;
  }
  return tcp_conn->WriteData(buffer, size);
}

}  // namespace net_stack
