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
#include "PacketQueue.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

namespace net_stack {

class Host {
 public:
  Host(const std::string& hostname, const std::string& ip_address,
       BaseChannel* channel);
  ~Host();

  // This is the callback passed to channel. Channel use it to move packets
  // to host's receive queue.
  void MovePacketsFromChannel(std::queue<std::unique_ptr<Packet>>* new_pkts);

  // This is called by each TCP connection to deliver packets to host's
  // send queue.
  void MultiplexPacketsFromTcp(
      std::queue<std::unique_ptr<Packet>>* packets_to_send);

  // TODO: Make this private.
  void CreateTcpConnection(const std::string& source_ip, uint32 source_port,
                           uint32 local_port, uint32 socket_fd);

  // Remove a tcp connection and release all its resource.
  void DeleteTcpConnection(const TcpControllerKey& tcp_key);

  // Read and write data with socket.
  int32 ReadData(int32 socket_fd, byte* buffer, int32 size);
  int32 WriteData(int32 socket_fd, const byte* buffer, int32 size);

  // "System Calls".
  int32 Socket();

  bool Bind(int32 sock_fd, const std::string& local_ip, uint32 local_port);
  bool Connect(int32 sock_fd,
               const std::string& remote_ip, uint32 remote_port);
  bool Listen(int32 sock_fd);
  int Accept(int32 sock_fd);

  bool Close(int32 sock_fd);

  std::string hostname() const { return hostname_; }
  std::string ip_address() const { return ip_address_; }

 private:
  void Initialize();

  void PacketsReceiveListener();
  void DemultiplexPacketsToTcps(
      std::queue<std::unique_ptr<Packet>>* new_packets);

  void PacketsSendListener();

  bool HandleNewConnection(const Packet& pkt);

  // Get next available file descriptor.
  int32 GetFileDescriptor();
  void ReleaseFileDescriptor(int32 fd);

  // Port resource management.
  uint32 GetRandomPort();
  void ReleasePort(uint32 port);

  void debuginfo(const std::string& msg);

  std::string hostname_;
  std::string ip_address_;  // Human readable IP address (aa.bb.cc.dd)
  BaseChannel* channel_;  // This is the channel to send packets.

  // Receive packets queue.
  PacketQueue recv_pkt_queue_;

  // Send packets queue.
  PacketQueue send_pkt_queue_;

  // All TCP connections maintained by this host.
  using TcpConnectionMap = 
      std::unordered_map<TcpControllerKey, std::unique_ptr<TcpController>>;
  TcpConnectionMap connections_;
  std::mutex connections_mutex_;

  // This map maintains socket fd --> TcpController
  std::unordered_map<int32, TcpController*> socket_tcp_map_;
  std::mutex socket_tcp_map_mutex_;

  // All available file descriptors.
  std::set<int32> fd_pool_;
  std::mutex fd_pool_mutex_;

  // All available ports.
  std::set<uint32> port_pool_;
  std::mutex port_pool_mutex_;

  // File descriptors that have been bound.
  std::unordered_map<int32, LocalLayerThreeKey> bound_fds_;
  std::mutex bound_fds_mutex_;

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

  // Thread pool.
  Executors::FixedThreadPool thread_pool_;
};

}  // namespace net_stack

#endif  // NET_STACK_HOST_
