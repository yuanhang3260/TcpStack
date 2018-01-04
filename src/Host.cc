#include "Base/Ptr.h"
#include "Base/Log.h"
#include "Base/Utils.h"
#include "Host.h"
#include "Utility/CleanUp.h"

namespace net_stack {

namespace {
// Process file descriptor available in [3, 64).
const int32 kMinFd = 3;
const int32 kMaxFd = 63;
// Open file id available in [0, 256).
const int32 kMinOpenFileId = 0;
const int32 kMaxOpenFileId = 255;
// Port available in [1024, 1024 + 128).
const int32 kMinPort = 1024;
const int32 kMaxPort = 1151;
}

// ***************************** Process ************************************ //
Process::Process(Host* host) : host_(host) {
  fd_pool_ = ptr::MakeUnique<NumPool>(kMinFd, kMaxFd);
}

Process::~Process() {}

int32 Process::FindFdMappedFileId(int32 fd) {
  // Find kernel open file entry this file descriptor maps to.
  std::unique_lock<std::mutex> lock(fd_table_mutex_);
  auto it = fd_table_.find(fd);
  if (it == fd_table_.end()) {
    LogERROR("Invalid fd %d, not allocated");
    return -1;
  }
  int32 open_file_id = it->second;
  return open_file_id;
}

int32 Process::Socket() {
  // Allocate a new file descriptor.
  int32 fd = fd_pool_.Allocate();
  if (fd < 0) {
    return -1;
  }

  // Create socket in kernel and return open file table entry id.
  int open_file_id = host_->CreateNewSocket().first;
  if (open_file_id < 0) {
    return -1;
  }

  // Update process fd table.
  std::unique_lock<std::mutex> lock(fd_table_mutex_);
  fd_table_->emplace(fd, open_file_id);
  return fd;
}

bool Process::Bind(int32 socket_fd,
                   const std::string& local_ip, uint32 local_port) {
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->SocketBind(open_file_id, local_ip, local_port);
}

bool Process::Listen(int32 socket_fd) {
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->SocketListen(open_file_id);
}

int Process::Accept(int32 listen_socket) {
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }

  // Wait for accept new connection, and kernel will return a new open file
  // id associated with a newly created socket. Process should allocate a new
  // file descriptor and map it to the new open file.
  int32 new_open_file_id = host_->SocketAccept(open_file_id);
  int32 new_fd = fd_pool_.Allocate();
  if (new_fd < 0) {
    // TODO: What happens if allocating new fd fails? Kernel will still keep
    // the socket object in open file table, but no process fd will map to it.
    // The open file will be ab dangling entry in table. So we should close
    // this socket and connection in kernel. Kernel will immediately send a RST
    // to the TCP connection.

    // SendBackRST(tcp_key);
    return -1;
  }

  std::unique_lock<std::mutex> lock(fd_table_mutex_);
  fd_table_->emplace(new_fd, new_open_file_id);
  debuginfo("Accept fd = " + std::to_string(new_fd));
  return new_fd;
}

bool Process::Connect(int32 socket_fd,
                      const std::string& remote_ip, uint32 remote_port) {
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }

  return host_->SocketConnect(open_file_id, remote_ip, remote_port);
}

// ******************************* Host ************************************* //
Host::Host(const std::string& hostname, const std::string& ip_address,
           BaseChannel* channel) :
    hostname_(hostname),
    local_ip_address_(ip_address),
    channel_(channel),
    thread_pool_(5) {
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
  open_file_id_pool_ = ptr::MakeUnique<NumPool>(kMinOpenFileId, kMaxOpenFileId);
  port_pool_ = ptr::MakeUnique<NumPool>(kMinPort, kMaxPort);
}

Socket* Host::GetSocket(int32 open_file_id) {
  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  auto it = open_files_table_.find(open_file_id);
  if (it == open_files_table_.end()) {
    LogERROR("Could not find open file id %d", open_file_id);
    return nullptr;
  }

  if (it->second->type != KernelOpenFile::SOCKET) {
    LogERROR("Could not bind port %d to open file id %d which is not a socket");
    return nullptr;
  }

  return &(it->second->socket);
}

bool Host::GetSocketLocalBound(int32 open_file_id, LocalLayerThreeKey* key) {
  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return false;
  }

  if (!socket->isBound()) {
    LogERROR("Could not listen on socket which is not a bound to any port");
    return false;
  }
  *key = socket->local_bound;
  return true
}

std::pair<int32, Socket*> Host::CreateNewSocket() {
  int32 id = open_file_id_pool_.Allocate();
  if (id < 0) {
    return std::make_pair<int32, Socket*>(-1, nullptr);
  }

  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  open_files_table_.emplace(id, ptr::MakeUnique<KernelOpenFile>(SOCKET));
  open_files_table_.at(id)->IncRef();
  return std::make_pair<int32, Socket*>(id, open_files_table_.at(id).get());
}

bool Host::SocketBind(int32 open_file_id,
                      const std::string& local_ip, uint32 local_port) {
  if (!port_pool_.Take(local_port)) {
    LogERROR("Could not bind to local_port %d which is already being used.");
    return false;
  }

  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return false;
  }

  if (socket->isBound()) {
    LogERROR("Could not bind open file id %d which is already bound to port %d",
             open_file_id, socket->local_bound.local_port);
    return false;
  }

  socket->Bind(local_ip, local_port);
  return true;
}

bool Host::SocketListen(int32 open_file_id) {
  LocalLayerThreeKey local_listener_key;
  if (!GetSocketLocalBound(open_file_id, &local_listener_key)) {
    return false;
  }

  // Insert {local_ip, local_port} to listener map.
  std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
  std::unique_ptr<Listener> listener(new Listener);
  listeners_.emplace(local_listener_key, std::move(listener));
}

bool Host::SocketAccept(int32 open_file_id) {
  // Get socket listener.
  Listener* listener = nullptr;
  {
    std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
    auto it = listeners_.find(local_listener_key);
    if (it == listeners_.end()) {
      LogERROR("Socket is not in listening mode");
      return false;
    }
    listener = it->second.get();
  }

  // There is incoming connection to this listening socket.
  std::unique_lock<std::mutex> listener_lock(listener->mutex);
  listener->cv.wait(listener_lock,
                    [&] {return !listener->tcp_sockets.empty(); });
  int open_file_id = listener->tcp_sockets.front();
  listener->tcp_sockets.pop();
  return open_file_id;
}

bool Host::SocketConnect(int32 open_file_id,
                         const std::string& remote_ip, uint32 remote_port) {
  // Check if this socket is bound to local port. If not, assign a random port
  // and bind to it.
  LocalLayerThreeKey local_key;
  Socket* socket = GetSocket(open_file_id);
  if (!socket->isBound()) {
    local_key = LocalLayerThreeKey{local_ip_address_,
                                   port_pool_.AllocateRandom()};
    socket->Bind(local_key);
  } else {
    local_key = socket->local_bound;
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
    port_pool_->Release(local_key.local_port);
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
    std::unique_lock<std::mutex> connections_lock(connections_mutex_);
    auto it = connections_.find(tcp_key);
    if (it == connections_.end()) {
      connections_lock.unlock();
      if (!HandleNewConnection(*pkt)) {
        LogERROR("%s: Can't find tcp connection %s",
                 hostname_.c_str(), tcp_key.DebugString().c_str());
        // Send RST to the other side.
        // But first check this packet is not an RST! Otherwise both sides will
        // infinitely repeat sending RST to each other.
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
  auto new_socket = CreateNewSocket();
  int open_file_id = new_socket.first;
  if (open_file_id < 0) {
    LogERROR("Could not create socket for new TCP connectoin.");
    return false;
  }
  Socket* socket = new_socket.second;

  // Create TCP connection object.
  auto tcp_options = TcpController::GetDefaultOptions();
  // Set recv_base = sender's SYNC seq_num. This marks I'm expecting to receive
  // the first packet with exactly this seq_num. Now client to server connection
  // is established.
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
  listener->tcp_sockets.push(open_file_id);
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
  std::unique_ptr<TcpController> tcp_con = std::move(it->second);
  connections_.erase(it);
  connections_lock.unlock();

  // Wait for this connection is ready to be deleted.
  tcp_con->WaitForReadyToDestroy();

  // Reset the socket. It now can be bound to other TCP connections (e.g. call
  // Connect on the fd to create new TCP connection).
  int32 socket_fd = tcp_con->socket_fd();
  if (socket_fd > 0) {
    std::unique_lock<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(socket_fd);
    if (it != sockets_.end()) {
      it->second->tcp_con = nullptr;
      it->second->state = OPEN;
    }
  }

  // Release port.
  port_pool_.Release(tcp_key.dest_port);

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

void Host::debuginfo(const std::string& msg) {
  LogINFO((hostname_ + ": " + msg).c_str());
}

}  // namespace net_stack
