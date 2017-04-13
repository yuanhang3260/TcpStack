#include "Base/Ptr.h"
#include "Base/Log.h"
#include "Base/Utils.h"
#include "Host.h"
#include "Utility/CleanUp.h"

namespace net_stack {

Host::Host(const std::string& hostname, const std::string& ip_address,
           BaseChannel* channel) :
    hostname_(hostname),
    ip_address_(ip_address),
    channel_(channel),
    thread_pool_(2) {
  Initialize();

  thread_pool_.AddTask(std::bind(&Host::PacketsReceiveListener, this));
  thread_pool_.AddTask(std::bind(&Host::PacketsSendListener, this));
  thread_pool_.Start();
}

Host::~Host() {
  recv_pkt_queue_.Stop();
  send_pkt_queue_.Stop();
}

void Host::Initialize() {
  // Init file descriptor pool. We only reserve [3, 64) file descriptors.
  for (uint32 i = 3; i < 64; i++) {
    fd_pool_.insert(i);
  }

  // Init port pool. We only reserve [1024, 1024 + 128) ports.
  for (uint32 i = 1024; i < 1024 + 128; i++) {
    port_pool_.insert(i);
  }
}

void Host::MovePacketsFromChannel(
    std::queue<std::unique_ptr<Packet>>* packets_to_receive) {
  recv_pkt_queue_.Push(packets_to_receive);
  SANITY_CHECK(packets_to_receive->empty(), "packets_to_receive not emptied");
}

void Host::PacketsReceiveListener() {
  while (true) {
    std::queue<std::unique_ptr<Packet>> new_packets;
    recv_pkt_queue_.DeQueueAllTo(&new_packets);
    DemultiplexPacketsToTcps(&new_packets);
  }
}

void Host::DemultiplexPacketsToTcps(
    std::queue<std::unique_ptr<Packet>>* new_packets) {
  uint32 size = new_packets->size();
  for (uint32 i = 0; i < size; i++) {
    auto& pkt = new_packets->front();
    TcpControllerKey tcp_key{pkt->ip_header().dest_ip,
                             pkt->tcp_header().dest_port,
                             pkt->ip_header().source_ip,
                             pkt->tcp_header().source_port};
    auto it = connections_.find(tcp_key);
    if (it == connections_.end()) {
      if (!HandleNewConnection(*pkt)) {
        LogERROR("%s: Can't find tcp connection %s, "
                 "and neither is there any listener on {%s, %u}",
                 hostname_.c_str(), tcp_key.DebugString().c_str(),
                 pkt->ip_header().source_ip.c_str(),
                 pkt->tcp_header().source_port);
        continue;
      }
    }
    connections_.at(tcp_key)->ReceiveNewPacket(std::move(pkt));
    new_packets->pop();
  }
}

bool Host::HandleNewConnection(const Packet& pkt) {
  if (!pkt.tcp_header().sync) {
    LogERROR("Not a sync packet, skip creating new tcp connection");
    return false;
  }

  // Check if any socket is listening on this port.
  LocalLayerThreeKey key{pkt.ip_header().source_ip,
                         pkt.tcp_header().source_port};
  std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
  auto it = listeners_.find(key);
  if (it == listeners_.end()) {
    return false;
  }
  Listener* listener = it->second.get();
  listeners_lock.unlock();

  // Create a new TCP connection.
  TcpControllerKey tcp_key{pkt.ip_header().dest_ip,
                           pkt.tcp_header().dest_port,
                           pkt.ip_header().source_ip,
                           pkt.tcp_header().source_port};

  int32 new_fd = GetFileDescriptor();
  auto tcp_options = TcpController::GetDefaultOptions();
  // Set recv_base = sender's seq_num. This marks I'm expecting to receive the
  // first packet with exactly this seq_num. Client to server connection starts!
  tcp_options.recv_window_base = pkt.tcp_header().seq_num;

  connections_.emplace(tcp_key,
                       ptr::MakeUnique<TcpController>(
                          this, tcp_key, new_fd, tcp_options));
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    socket_tcp_map_.emplace(new_fd, connections_.at(tcp_key).get());
  }

  // Notify Accept() to get the a new TCP connection socket.
  std::unique_lock<std::mutex> listener_lock(listener->mutex);
  listener->tcp_sockets.push(new_fd);
  listener->cv.notify_one();
  return true;
}

void Host::MultiplexPacketsFromTcp(
    std::queue<std::unique_ptr<Packet>>* packets_to_send) {
  send_pkt_queue_.Push(packets_to_send);
  SANITY_CHECK(packets_to_send->empty(), "packets_to_send not emptied!");
}

void Host::PacketsSendListener() {
  while (true) {
    std::queue<std::unique_ptr<Packet>> packets_to_send;
    send_pkt_queue_.DeQueueAllTo(&packets_to_send);
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

int32 Host::ReadData(int32 socket_fd, byte* buffer, int32 size) {
  TcpController* tcp_con;
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    auto it = socket_tcp_map_.find(socket_fd);
    if (it == socket_tcp_map_.end()) {
      LogERROR("Can't find tcp connection bind with socket %u", socket_fd);
      return -1;
    }
    tcp_con = it->second;
  }
  return tcp_con->ReadData(buffer, size);
}

int32 Host::WriteData(int32 socket_fd, const byte* buffer, int32 size) {
  TcpController* tcp_con;
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    auto it = socket_tcp_map_.find(socket_fd);
    if (it == socket_tcp_map_.end()) {
      LogERROR("Can't find tcp connection bind with socket %u", socket_fd);
      return -1;
    }
    tcp_con = it->second;
  }
  return tcp_con->WriteData(buffer, size);
}

int32 Host::Socket() {
  return GetFileDescriptor();
}

bool Host::Bind(int32 sock_fd,
                const std::string& local_ip, uint32 local_port) {
  std::unique_lock<std::mutex> lock(bound_fds_mutex_);
  auto it = bound_fds_.find(sock_fd);
  if (it != bound_fds_.end()) {
    LogERROR("fd %u already bound to {%s, %u}",
             sock_fd, it->second.local_ip.c_str(), it->second.local_port);
    return false;
  }

  bound_fds_.emplace(sock_fd, LocalLayerThreeKey{local_ip, local_port});
  return true;
}

bool Host::Listen(int32 sock_fd) {
  std::unique_lock<std::mutex> bound_fds_lock(bound_fds_mutex_);
  auto it = bound_fds_.find(sock_fd);
  if (it == bound_fds_.end()) {
    LogERROR("fd %u is not bound to any local {ip_address, port}");
    return false;
  }

  // insert {local_ip, local_port} to listener set.
  std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
  std::unique_ptr<Listener> listener(new Listener);
  listeners_.emplace(it->second, std::move(listener));
  return true;
}

int Host::Accept(int32 listen_sock) {
  std::unique_lock<std::mutex> bound_fds_lock(bound_fds_mutex_);
  auto it = bound_fds_.find(listen_sock);
  if (it == bound_fds_.end()) {
    LogERROR("fd %u is not bound to any local {ip_address, port}");
    return false;
  }
  LocalLayerThreeKey key = it->second;
  bound_fds_lock.unlock();

  std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
  auto it2 = listeners_.find(key);
  if (it2 == listeners_.end()) {
    LogERROR("Socket %d is not in listening mode", listen_sock);
    return -1;
  }
  listeners_lock.unlock();

  Listener* listener = it2->second.get();
  std::unique_lock<std::mutex> listener_lock(listener->mutex);
  listener->cv.wait(listener_lock,
                    [&] {return !listener->tcp_sockets.empty(); });
  int new_fd = listener->tcp_sockets.front();
  debuginfo("Accept fd = " + std::to_string(new_fd));
  listener->tcp_sockets.pop();
  return new_fd;
}

bool Host::Connect(int32 sock_fd,
                   const std::string& remote_ip, uint32 remote_port) {
  // Check if this socket is bound to local port. If not, assign a random port
  // to it.
  LocalLayerThreeKey local_key;
  {
    std::unique_lock<std::mutex> bound_fds_lock(bound_fds_mutex_);
    auto it = bound_fds_.find(sock_fd);
    if (it == bound_fds_.end()) {
      uint32 port = GetRandomPort();
      local_key = LocalLayerThreeKey{ip_address_, port};
      bound_fds_.emplace(sock_fd, local_key);
    } else {
      local_key = it->second;
    }
  }

  // Create TcpController and try establishing TCP connection with remote host.
  // Note remote host is the "source" part in TcpControllerKey.
  TcpControllerKey tcp_key{remote_ip, remote_port, 
                           local_key.local_ip, local_key.local_port};
  auto tcp_options = TcpController::GetDefaultOptions();
  tcp_options.send_window_base = 0;  /* Utils::RandomNumber(); */
  connections_.emplace(tcp_key,
                       ptr::MakeUnique<TcpController>(
                          this, tcp_key, sock_fd, tcp_options));
  auto tcp_con = connections_.at(tcp_key).get();

  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    socket_tcp_map_.emplace(sock_fd, tcp_con);
  }

  // Clean up steps.
  auto clean_up = Utility::CleanUp([&]() {
    ReleasePort(local_key.local_port);
  });

  // TcpController::TryConnect starts three-way handshake, by sending a SYNC
  // segment to remote host.
  bool re = tcp_con->TryConnect();
  if (!re) {
    return false;
  }

  clean_up.clear();
  return true;
}

bool Host::Close(int32 sock_fd) {
  TcpController* tcp_con = nullptr;
  {
    std::unique_lock<std::mutex> lock(socket_tcp_map_mutex_);
    auto it = socket_tcp_map_.find(sock_fd);
    if (it == socket_tcp_map_.end()) {
      LogERROR("Can't find tcp connection bind with socket %u", sock_fd);
      return -1;
    }
    tcp_con = it->second;
  }

  auto re = tcp_con->TryClose();
  if (!re) {
    // TODO: Close fail?
    return false;
  }
  return true;  
}

uint32 Host::GetRandomPort() {
  std::unique_lock<std::mutex> port_pool_lock(port_pool_mutex_);
  uint32 size = port_pool_.size();
  if (size == 0) {
    return 0;
  }
  
  // Select a random port from port pool.
  uint32 index = Utils::RandomNumber(size);
  auto it = port_pool_.begin();
  advance(it, index);
  uint32 port = *it;
  port_pool_.erase(it);
  return port;
}

void Host::ReleasePort(uint32 port) {
  std::unique_lock<std::mutex> port_pool_lock(port_pool_mutex_);
  port_pool_.insert(port);
}

int32 Host::GetFileDescriptor() {
  std::unique_lock<std::mutex> fd_pool_lock(fd_pool_mutex_);

  if (fd_pool_.empty()) {
    return -1;
  }

  // Get the first (smallest) file descriptor available.
  auto it = fd_pool_.begin();
  int32 fd = *it;
  fd_pool_.erase(it);
  return fd;
}

void Host::ReleaseFileDescriptor(int32 fd) {
  std::unique_lock<std::mutex> fd_pool_lock(fd_pool_mutex_);
  fd_pool_.insert(fd);
}

void Host::debuginfo(const std::string& msg) {
  LogINFO((hostname_ + ": " + msg).c_str());
}

}  // namespace net_stack
