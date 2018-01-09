#include "Strings/Utils.h"
#include "Utility/CleanUp.h"

#include "Base/Ptr.h"
#include "Base/Log.h"
#include "Base/Utils.h"

#include "debug.h"
#include "Host.h"

namespace net_stack {

namespace {

using Strings::StrCat;

// Process file descriptor available in [3, 64).
const int32 kMinFd = 3;
const int32 kMaxFd = 63;

// Open file id available in [0, 256).
const int32 kMinOpenFileId = 0;
const int32 kMaxOpenFileId = 255;

// Port available in [0, 128).
const int32 kMinPort = 0;
const int32 kMaxPort = 127;

}  // namespace


// ************************************************************************** //
// ***************************** Process ************************************ //
Process::Process(Host* host, const std::string& name, Program program) :
    host_(host),
    name_(name),
    program_(program) {
  fd_pool_ = ptr::MakeUnique<NumPool>(kMinFd, kMaxFd);
}

std::string Process::hostname() const {
  return host_->hostname();
}

void Process::Run() {
  program_(this);
  ReleaseResource();
}

void Process::ReleaseResource() {
  // Close all file descriptors. Note we first save all fds locally, because
  // Close() will delete fd entry from fd_table.
  std::vector<int32> fds;
  for (const auto& it : fd_table_) {
    fds.push_back(it.first);
  }
  for (int32 fd : fds) {
    LOG(StrCat("closing fd ", std::to_string(fd)));
    Close(fd);
  }
}

Process::~Process() {}

int32 Process::FindFdMappedFileId(int32 fd) {
  // Find kernel open file entry this file descriptor maps to.
  std::unique_lock<std::mutex> lock(fd_table_mutex_);
  auto it = fd_table_.find(fd);
  if (it == fd_table_.end()) {
    LOG(StrCat("Invalid fd ", std::to_string(fd),
                            ", not allocated / or maybe closed?"));
    return -1;
  }
  int32 open_file_id = it->second;
  return open_file_id;
}

int32 Process::Socket() {
  LOG("Socket()");
  // Allocate a new file descriptor.
  int32 fd = fd_pool_->Allocate();
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
  fd_table_.emplace(fd, open_file_id);
  return fd;
}

bool Process::Bind(int32 socket_fd,
                   const std::string& local_ip, uint32 local_port) {
  LOG("Bind()");
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->SocketBind(open_file_id, local_ip, local_port);
}

bool Process::Listen(int32 socket_fd) {
  LOG("Listen()");
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->SocketListen(open_file_id);
}

int Process::Accept(int32 socket_fd) {
  LOG("Accept()");
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }

  // Wait for accept new connection, and kernel will return a new open file
  // id associated with a newly created socket. Process should allocate a new
  // file descriptor and map it to the new open file.
  int32 new_open_file_id = host_->SocketAccept(open_file_id);
  int32 new_fd = fd_pool_->Allocate();
  if (new_fd < 0) {
    // What happens if allocating new fd fails? Kernel will still keep this
    // socket object in open file table, but no process fd will map to it.
    // The open file will be an dangling entry in table. So we should close
    // this socket and connection in kernel, and immediately send an RST to
    // the other side.
    host_->DeleteSocketAndConnection(open_file_id);
    return -1;
  }

  std::unique_lock<std::mutex> lock(fd_table_mutex_);
  fd_table_.emplace(new_fd, new_open_file_id);
  LOG("Accept fd = " + std::to_string(new_fd));
  return new_fd;
}

bool Process::Connect(int32 socket_fd,
                      const std::string& remote_ip, uint32 remote_port) {
  LOG("Connect()");
  int32 open_file_id = FindFdMappedFileId(socket_fd);
  if (open_file_id < 0) {
    return false;
  }

  return host_->SocketConnect(open_file_id, remote_ip, remote_port);
}

int32 Process::Read(int32 fd, byte* buffer, int32 size) {
  LOG("Read()");
  int32 open_file_id = FindFdMappedFileId(fd);
  if (open_file_id < 0) {
    // Bad file descriptor error. It's closed and not mapped to any file/socket.
    return false;
  }
  return host_->ReadData(open_file_id, buffer, size);
}

int32 Process::Write(int32 fd, const byte* buffer, int32 size) {
  LOG("Write()");
  int32 open_file_id = FindFdMappedFileId(fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->WriteData(open_file_id, buffer, size);
}

bool Process::ShutDown(int32 fd) {
  LOG("Shutdown()");
  int32 open_file_id = FindFdMappedFileId(fd);
  if (open_file_id < 0) {
    return false;
  }
  return host_->ShutDownSocket(open_file_id);
}

bool Process::Close(int32 fd) {
  LOG("Close()");
  // Close() is different from ShutDown(). Close() just closes the process
  // file descriptor that maps to the open file entry of this socket. It will
  // decrease the open file reference count. When reference count becomes zero,
  // TCP socket will be shutdown, by sending a FIN to the other side.
  //
  // Of course, process itself should recycle the fd when Close() is called.
  int32 open_file_id = FindFdMappedFileId(fd);
  if (open_file_id < 0) {
    return false;
  }

  // This will decrease socket ref count in open file table, and if ref count
  // is zero, will close TCP connection and delete open file table entry. 
  host_->DecOpenFileRef(open_file_id);

  // Remove fd from fd_table and release the file descriptor.
  {
    std::unique_lock<std::mutex> lock(fd_table_mutex_);
    auto it = fd_table_.find(fd);
    if (it == fd_table_.end()) {
      return false;
    }
    fd_table_.erase(it);
  }

  fd_pool_->Release(fd);
  return true;
}

// ************************************************************************** //
// ******************************* Host ************************************* //
Host::Host(const std::string& hostname, const std::string& ip_address,
           BaseChannel* channel) :
    hostname_(hostname),
    local_ip_address_(ip_address),
    channel_(channel),
    thread_pool_(20) {
  Initialize();

  thread_pool_.AddTask(std::bind(&Host::PacketsReceiveListener, this));
  thread_pool_.AddTask(std::bind(&Host::PacketsSendListener, this));
}

Host::~Host() {
  recv_pkt_queue_.Stop();
  send_pkt_queue_.Stop();
}

void Host::RunForever() {
  thread_pool_.Start();
}

void Host::Initialize() {
  open_file_id_pool_ = ptr::MakeUnique<NumPool>(kMinOpenFileId, kMaxOpenFileId);
  port_pool_ = ptr::MakeUnique<NumPool>(kMinPort, kMaxPort);
}

bool Host::CreateProcess(const std::string& name, Program program) {
  std::unique_lock<std::mutex> lock(processes_mutex_);
  processes_.emplace(name, ptr::MakeUnique<Process>(this, name, program));
  Process* process = processes_.at(name).get();

  // Run the process.
  thread_pool_.AddTask(std::bind(&Process::Run, process));
  return true;
}

Socket* Host::GetSocket(int32 open_file_id) {
  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  auto it = open_files_table_.find(open_file_id);
  if (it == open_files_table_.end()) {
    LogERROR("Could not find open file id %d", open_file_id);
    return nullptr;
  }

  if (it->second->type != KernelOpenFile::SOCKET) {
    LogERROR("Could not get socket from open file id %d which is not a socket");
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
  return true;
}

int32 Host::DecOpenFileRef(int32 open_file_id) {
  // We keep the big lock of entire function, and we don't lock for single
  // table entries that is being operated on. This is neccessary because we
  // need to rule out any race condition when looking at the reference count
  // of the table entry.
  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  auto it = open_files_table_.find(open_file_id);
  if (it == open_files_table_.end()) {
    LogERROR("Could not find open file id %d", open_file_id);
    return -1;
  }

  if (it->second->Refs() <= 1) {
    Socket* socket = &it->second->socket;

    // Ref count is zero, close the TCP connection. Closing is bi-directional.
    // A closed socket will never be able to receive data any more. So the
    // mapped TCP connection should be notified of that fact. From now on,
    // if this TCP connection's receive buffer is or becomes non-empty, TCP
    // connection should send RST immediately to the other side to indicate a
    // closed connection. These are all implemented in TcpController::TryClose.
    if (socket->tcp_con != nullptr) {
      TcpController* tcp_con = socket->tcp_con;
      if (socket->isBound()) {
        tcp_con->SetReleasePort(true);
      }
      tcp_con->TryClose();
    } else {
      // If the TCP connection bound to this socket has been closed, release the
      // port of socket has bound. Otherwise, TCP connection connection itself
      // should be responsible for port releasing.
      if (socket->isBound()) {
        ReleasePort(socket->local_bound.local_port);
      }
    }

    // Delete socket from open file table.
    open_files_table_.erase(it);
    return 0;
  } else {
    it->second->DecRef();
    return it->second->Refs();
  }
}

int32 Host::IncOpenFileRef(int32 open_file_id) {
  // Same as DecOpenFileRef.
  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  auto it = open_files_table_.find(open_file_id);
  if (it == open_files_table_.end()) {
    LogERROR("Could not find open file id %d", open_file_id);
    return false;
  }

  it->second->IncRef();
  return it->second->Refs();
}

std::pair<int32, Socket*> Host::CreateNewSocket() {
  int32 id = open_file_id_pool_->Allocate();
  if (id < 0) {
    return std::make_pair<int32, Socket*>(-1, nullptr);
  }

  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  open_files_table_.emplace(
      id, ptr::MakeUnique<KernelOpenFile>(KernelOpenFile::SOCKET));
  open_files_table_.at(id)->IncRef();
  return std::make_pair(id, &open_files_table_.at(id)->socket);
}

bool Host::SocketBind(int32 open_file_id,
                      const std::string& local_ip, uint32 local_port) {
  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return false;
  }

  if (socket->isBound()) {
    LogERROR("Could not bind open file id %d which is already bound to port %d",
             open_file_id, socket->local_bound.local_port);
    return false;
  }

  if (!port_pool_->Take(local_port)) {
    LogERROR("local_port %d is already being used.", local_port);
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
  return true;
}

int32 Host::SocketAccept(int32 open_file_id) {
  LocalLayerThreeKey local_listener_key;
  if (!GetSocketLocalBound(open_file_id, &local_listener_key)) {
    return false;
  }

  // Get socket listener.
  Listener* listener = nullptr;
  {
    std::unique_lock<std::mutex> listeners_lock(listeners_mutex_);
    auto it = listeners_.find(local_listener_key);
    if (it == listeners_.end()) {
      LogERROR("Socket is not in listening mode");
      return -1;
    }
    listener = it->second.get();
  }

  // There is incoming connection to this listening socket.
  std::unique_lock<std::mutex> listener_lock(listener->mutex);
  listener->cv.wait(listener_lock,
                    [&] {return !listener->tcp_sockets.empty(); });
  int new_open_file_id = listener->tcp_sockets.front();
  listener->tcp_sockets.pop();
  return new_open_file_id;
}

bool Host::SocketConnect(int32 open_file_id,
                         const std::string& remote_ip, uint32 remote_port) {
  // Check if this socket is bound to local port. If not, assign a random port
  // and bind to it.
  LocalLayerThreeKey local_key;
  Socket* socket = GetSocket(open_file_id);
  if (!socket->isBound()) {
    local_key = LocalLayerThreeKey{local_ip_address_,
                                   port_pool_->AllocateRandom()};
    socket->Bind(local_key);
  } else {
    local_key = socket->local_bound;
  }

  // Create TcpController and try establishing TCP connection with remote host.
  // Note remote host is the "source" part in TcpControllerKey.
  TcpControllerKey tcp_key(remote_ip, remote_port, 
                           local_key.local_ip, local_key.local_port);
  auto tcp_options = TcpController::GetDefaultOptions();
  tcp_options.send_window_base = 0;  /* Utils::RandomNumber(); */
  connections_.emplace(tcp_key,
                       ptr::MakeUnique<TcpController>(
                          this, socket, tcp_key, tcp_options));
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
        LogINFO("%s: Can't find tcp connection %s",
                hostname_.c_str(), tcp_key.DebugString().c_str());
        // Send RST to the other side.
        // But first check this packet is not an RST! Otherwise both sides will
        // infinitely repeat sending RST to each other.
        if (!pkt->tcp_header().rst) {
          LOG("Host sending back RST")
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
    // Not a sync packet, discard it.
    return false;
  }

  // Check if any socket is listening on this port.
  LocalLayerThreeKey key(pkt.ip_header().source_ip,
                         pkt.tcp_header().source_port);
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
                            this, socket, tcp_key, tcp_options));
    // Note this socket/TCP connection should not bind to any port, though it
    // does have a tcp_key. On server side, only the listener socket is bounded.
    socket->tcp_con = connections_.at(tcp_key).get();

    // Deliver the SYN packet to this TCP connection so that it will immediately
    // reply SYN_ACK back.
    socket->tcp_con->ReceiveNewPacket(std::unique_ptr<Packet>(pkt.Copy()));
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
    // Block reading from packet send queue.
    send_pkt_queue_.DeQueueAllTo(&packets_to_send);
    // Send to channel.
    channel_->Send(&packets_to_send);
  }
}

void Host::DeleteSocketAndConnection(int32 open_file_id) {
  TcpControllerKey key;
  std::unique_lock<std::mutex> lock(open_files_table_mutex_);
  auto it = open_files_table_.find(open_file_id);
  if (it == open_files_table_.end()) {
    return;
  }
  if (it->second->socket.tcp_con != nullptr) {
    key = it->second->socket.tcp_con->key();
  }
  open_files_table_.erase(it);
  lock.unlock();

  if (!key.dest_ip.empty() && key.dest_port > 0) {
    DeleteTcpConnection(key);
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
  connections_.erase(it);
  connections_lock.unlock();

  LOG(Strings::StrCat(
      "Connection ", tcp_key.DebugString(), " safely deleted ^_^"));
}

int32 Host::ReadData(int32 open_file_id, byte* buffer, int32 size) {
  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return -1;
  }
  TcpController* tcp_con = socket->tcp_con;
  return tcp_con->ReadData(buffer, size);
}

int32 Host::WriteData(int32 open_file_id, const byte* buffer, int32 size) {
  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return -1;
  }

  // Prevent sending data after socket is closed or shutdown. Note this check
  // resides in socket layer rather TCP layer.
  if (socket->state == Socket::SHUTDOWN) {
    LogERROR("Socket is shut down, can't send more data");
    return -1;
  }

  TcpController* tcp_con = socket->tcp_con;
  if (tcp_con == nullptr) {
    // This socket has no TCP connection. This can happen when writing to a
    // socket which has TCP connection already closed by the other side's RST.
    // In real Linux system, kernel will send a SIGPIPE signal to process and
    // the default behavior is terminating the process. Here we return -2
    // to indicate this error. It should better be an enum, but I'm lazy.
    return -2;
  }

  return tcp_con->WriteData(buffer, size);
}

bool Host::ShutDownSocket(int32 open_file_id) {
  Socket* socket = GetSocket(open_file_id);
  if (socket == nullptr) {
    return -1;
  }
  if (socket->state == Socket::SHUTDOWN) {
    return true;
  }

  TcpController* tcp_con = socket->tcp_con;
  socket->state = Socket::SHUTDOWN;

  // TCP connection shutdown - It sends FIN to the other side to indicate end
  // of sending data. The socket is not closed, and process file descriptor can
  // still be used to read data from this socket.
  auto re = tcp_con->TryShutDown();
  if (!re) {
    // TODO: Shutdown fail?
    return false;
  }

  return true;
}

void Host::ReleasePort(uint32 port) {
  LOG("release port %d", port);
  port_pool_->Release(port);
}

}  // namespace net_stack
