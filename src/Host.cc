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
                 "and neither is there any listener on {%s:%u}",
                 hostname_.c_str(), tcp_key.DebugString().c_str(),
                 pkt->ip_header().source_ip.c_str(),
                 pkt->tcp_header().source_port);
        // Send RST to the other side.
        if (!pkt->tcp_header().rst) {
          SendBackRST(tcp_key);
        }
        continue;
      }
    }
    connections_.at(tcp_key)->ReceiveNewPacket(std::move(pkt));
    new_packets->pop();
  }
}

void Host::SendBackRST(TcpControllerKey tcp_key) {
  IPHeader ip_header;
  ip_header.source_ip = tcp_key.source_ip;
  ip_header.dest_ip = tcp_key.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = tcp_key.source_port;
  tcp_header.dest_port = tcp_key.dest_port;
  tcp_header.rst = true;

  std::unique_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  send_pkt_queue_.Push(std::move(pkt));
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

  // Create Socket object.
  int32 new_fd = GetFileDescriptor();
  std::shared_ptr<net_stack::Socket> socket(new net_stack::Socket(new_fd));
  {
    std::unique_lock<std::mutex> sockets_lock(sockets_mutex_);
    sockets_.emplace(new_fd, socket);
  }

  // Create TCP connection object.
  auto tcp_options = TcpController::GetDefaultOptions();
  // Set recv_base = sender's seq_num. This marks I'm expecting to receive the
  // first packet with exactly this seq_num. Client to server connection starts!
  tcp_options.recv_window_base = pkt.tcp_header().seq_num;

  {
    std::unique_lock<std::mutex> connections_lock(connections_mutex_);
    connections_.emplace(tcp_key,
                         ptr::MakeUnique<TcpController>(
                            this, tcp_key, socket, tcp_options));
    socket->tcp_con = connections_.at(tcp_key).get();
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

void Host::DeleteTcpConnection(const TcpControllerKey& tcp_key) {
  // Remove from connection map.
  std::unique_lock<std::mutex> connections_lock(connections_mutex_);
  auto it = connections_.find(tcp_key);
  if (it == connections_.end()) {
    LogERROR("Can't find TCP connection %s, skip deleting",
             tcp_key.DebugString().c_str());
    return;
  }
  connections_lock.unlock();

  // Wait for this connection is ready to be deleted. Don't wait inside any
  // lock of host.
  it->second->WaitForReadyToDestroy();

  // Reset the socket. It now can be bound to other TCP connections (e.g. call
  // Connect on the fd to create new TCP connection).
  int32 socket_fd = it->second->socket_fd();
  if (socket_fd > 0) {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(socket_fd);
    if (it != sockets_.end()) {
      it->second->tcp_con = nullptr;
      it->second->state = OPEN;
    }
  }

  // Delete connection object.
  connections_lock.lock();
  connections_.erase(it);
  connections_lock.unlock();

  // Release port.
  ReleasePort(tcp_key.dest_port);

  debuginfo(Strings::StrCat(
      "Connection ", tcp_key.DebugString(), " safely deleted ^_^"));
}

int32 Host::ReadData(int32 socket_fd, byte* buffer, int32 size) {
  TcpController* tcp_con;
  SocketState socket_state;
  {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(socket_fd);
    if (it == sockets_.end()) {
      LogERROR("Can't find socket %d", socket_fd);
      return -1;
    }
    socket_state = it->second->state;
    tcp_con = it->second->tcp_con;
  }

  // If socket state is closed, prevent user reading data from this socket.
  // Note this check resides in socket layer.

  // After FIN is sent, TCP layer's behavior on receiving new data depends on
  // socket state. TCP layer needs to look at whether FIN is triggered by
  // Close() or ShutDown(), by looking at current socket state. If FIN is sent
  // by Close(), this TCP connection has no binding to user space anymore,
  // and it should reply RST to the other side, indicating no more data is
  // acceptable. If FIN is sent by ShutDown(), TCP connection can still receive
  // data until the other side sends a FIN.
  if (socket_state == CLOSED) {
    LogERROR("Socket has been closed, can't recv data");
    return -1;
  }

  return tcp_con->ReadData(buffer, size);
}

int32 Host::WriteData(int32 socket_fd, const byte* buffer, int32 size) {
  TcpController* tcp_con;
  SocketState socket_state;
  {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(socket_fd);
    if (it == sockets_.end()) {
      LogERROR("Can't find socket %d", socket_fd);
      return -1;
    }
    socket_state = it->second->state;
    tcp_con = it->second->tcp_con;
  }

  // Prevent sending data after socket is closed or shutdown. Note this check
  // resides in socket layer rather TCP layer. As a double check, TCP also
  // checks TCP state and blocks sending data if FIN is already sent.
  if (socket_state == SHUTDOWN || socket_state == CLOSED) {
    LogERROR("Socket has been shut down, can't send data");
    return -1;
  }

  return tcp_con->WriteData(buffer, size);
}

int32 Host::Socket() {
  int32 fd = GetFileDescriptor();
  std::unique_lock<std::mutex> lock(sockets_mutex_);
  sockets_.emplace(fd, ptr::MakeUnique<net_stack::Socket>(fd));
  return fd;
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

  std::shared_ptr<net_stack::Socket> socket;
  {
    std::unique_lock<std::mutex> sockets_lock(sockets_mutex_);
    auto it = sockets_.find(sock_fd);
    if (it == sockets_.end()) {
      LogERROR("fd %d is not socket", sock_fd);
      return false;
    }
    socket = it->second;
  }

  // Create TcpController and try establishing TCP connection with remote host.
  // Note remote host is the "source" part in TcpControllerKey.
  TcpControllerKey tcp_key{remote_ip, remote_port, 
                           local_key.local_ip, local_key.local_port};
  auto tcp_options = TcpController::GetDefaultOptions();
  tcp_options.send_window_base = 0;  /* Utils::RandomNumber(); */
  connections_.emplace(tcp_key,
                       ptr::MakeUnique<TcpController>(
                          this, tcp_key, socket, tcp_options));
  auto tcp_con = connections_.at(tcp_key).get();
  socket->tcp_con = tcp_con;

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

bool Host::ShutDown(int32 sock_fd) {
  TcpController* tcp_con = nullptr;
  {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(sock_fd);
    if (it == sockets_.end()) {
      LogERROR("Can't find socket %u", sock_fd);
      return false;
    }
    auto socket = it->second.get();
    tcp_con = socket->tcp_con;
    socket->state = SHUTDOWN;
  }

  // Tcp connection shutdown - It sends FIN to the other side.
  auto re = tcp_con->TryShutDown();
  if (!re) {
    // TODO: Close fail?
    return false;
  }

  return true;
}

bool Host::Close(int32 sock_fd) {
  // Close() is different with ShutDown() in that it closes both direction
  // communication of this socket.
  //
  // More importantly, it dec-couples the fd from TCP connection, which
  // means this TCP connection is no longer bound with any fd, and this
  // file descriptor will be recycled by system.
  TcpController* tcp_con = nullptr;
  {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(sock_fd);
    if (it == sockets_.end()) {
      LogERROR("Can't find socket %u in use", sock_fd);
      return false;
    }
    auto socket = it->second.get();
    tcp_con = socket->tcp_con;
    socket->state = CLOSED;
    socket->fd = -1;

    // Remove the socket from (fd --> socket) map. This fd will be released
    // in the following ReleaseFileDescriptor. Now only TCP connection itself
    // is still holding the reference of Socket, with socket state CLOSED.
    sockets_.erase(it);
  }

  // Release the file descriptor.
  ReleaseFileDescriptor(sock_fd);

  // Tcp connection shutdown - It sends FIN to the other side.
  auto re = tcp_con->TryShutDown();
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
