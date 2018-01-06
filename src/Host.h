#ifndef NET_STACK_HOST_
#define NET_STACK_HOST_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "Base/MacroUtils.h"
#include "BaseChannel.h"
#include "NumPool.h"
#include "PacketQueue.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

namespace net_stack {

class Host;
class Process;

// Program is the function that Process actually executes. Argument process is
// the system call interface.
using Program = std::function<void(Process* process)>;

class Process {
 public:
  Process(Host* host, const std::string& name, Program program);
  ~Process();

  void Run();

  // "System Calls".
  int32 Socket();
  bool Bind(int32 socket_fd,
            const std::string& local_ip, uint32 local_port);
  bool Listen(int32 socket_fd);
  int Accept(int32 listen_socket);
  bool Connect(int32 socket_fd,
               const std::string& remote_ip, uint32 remote_port);

  int32 Read(int32 fd, byte* buffer, int32 size);
  int32 Write(int32 fd, const byte* buffer, int32 size);

  bool ShutDown(int32 fd);
  bool Close(int32 fd);

 private:
  int32 FindFdMappedFileId(int32 fd);

  void ReleaseResource();

  // Kernel ref.
  Host* host_;
  std::string name_;
  Program program_;

  // All available file descriptors.
  std::unique_ptr<NumPool> fd_pool_;

  // Process file descriptor table.
  std::map<int32, int32> fd_table_;
  std::mutex fd_table_mutex_;
};

class Host {
 public:
  Host(const std::string& hostname, const std::string& ip_address,
       BaseChannel* channel);
  ~Host();

  void RunForever();

  // Create Process.
  bool CreateProcess(const std::string& name, Program program);

  // This is the callback passed to channel. Channel use it to move packets
  // to host's receive queue.
  void MovePacketsFromChannel(std::queue<std::unique_ptr<Packet>>* new_pkts);

  // This is called by each TCP connection to deliver packets to host's
  // send queue.
  void MultiplexPacketsFromTcp(
      std::queue<std::unique_ptr<Packet>>* packets_to_send);

  // Remove a tcp connection and release all its resource.
  void DeleteTcpConnection(const TcpControllerKey& tcp_key);

  // System calls implementations.
  std::pair<int32, Socket*> CreateNewSocket();

  bool SocketBind(int32 open_file_id,
                  const std::string& local_ip, uint32 local_port);

  bool SocketListen(int32 open_file_id);

  int32 SocketAccept(int32 open_file_id);

  bool SocketConnect(int32 open_file_id,
                     const std::string& remote_ip, uint32 remote_port);

  int32 ReadData(int32 open_file_id, byte* buffer, int32 size);
  int32 WriteData(int32 open_file_id, const byte* buffer, int32 size);

  bool ShutDownSocket(int32 open_file_id);

  std::string hostname() const { return hostname_; }
  std::string ip_address() const { return local_ip_address_; }

 private:
  void Initialize();

  Socket* GetSocket(int32 open_file_id);
  bool GetSocketLocalBound(int32 open_file_id, LocalLayerThreeKey* key);
  
  // Return reference count after function call.
  int32 DecOpenFileRef(int32 open_file_id);
  int32 IncOpenFileRef(int32 open_file_id);

  void PacketsReceiveListener();
  void DemultiplexPacketsToTcps(
      std::queue<std::unique_ptr<Packet>>* new_packets);

  void PacketsSendListener();

  bool HandleNewConnection(const Packet& pkt);

  void SendBackRST(TcpControllerKey tcp_key);

  // Port resource management.
  uint32 GetRandomPort();
  void ReleasePort(uint32 port);

  void debuginfo(const std::string& msg);

  std::string hostname_;
  std::string local_ip_address_;  // Human readable IP address (aa.bb.cc.dd)
  BaseChannel* channel_;  // This is the channel to send packets.

  // All processes.
  std::map<std::string, std::unique_ptr<Process>> processes_;
  std::mutex processes_mutex_;

  // Receive packets queue.
  PacketQueue recv_pkt_queue_;
  // Send packets queue.
  PacketQueue send_pkt_queue_;

  // Global open file table.
  std::unique_ptr<NumPool> open_file_id_pool_;
  std::map<int32, std::unique_ptr<KernelOpenFile>> open_files_table_;
  std::mutex open_files_table_mutex_;

  // All TCP connections maintained by this host.
  using TcpConnectionMap =
      std::unordered_map<TcpControllerKey, std::unique_ptr<TcpController>>;
  TcpConnectionMap connections_;
  std::mutex connections_mutex_;

  // Local listeners.
  struct Listener {
    // This is a queue of populated sockets assigned to each new TCP connection.
    // When this queue is not empty, an Accept() thread will be notified.
    std::queue<int32> tcp_sockets;
    std::mutex mutex;
    std::condition_variable cv;
  };
  std::unordered_map<LocalLayerThreeKey, std::unique_ptr<Listener>> listeners_;
  std::mutex listeners_mutex_;

  // All available ports.
  std::unique_ptr<NumPool> port_pool_;

  // Thread pool.
  Executors::FixedThreadPool thread_pool_;

  friend class Process;
};

}  // namespace net_stack

#endif  // NET_STACK_HOST_
