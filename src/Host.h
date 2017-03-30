#ifndef NET_STACK_HOST_
#define NET_STACK_HOST_

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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

  // Read and write data with socket.
  int32 ReadData(uint32 socket_fd, byte* buffer, int32 size);
  int32 WriteData(uint32 socket_fd, const byte* buffer, int32 size);

  std::string hostname() const { return hostname_; }
  std::string ip_address() const { return ip_address_; }

 private:
  void PacketsReceiveListener();
  void DemultiplexPacketsToTcps(
      std::queue<std::unique_ptr<Packet>>* new_packets);

  void PacketsSendListener();

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

  // This map maintains socket fd --> TcpController
  std::mutex socket_tcp_map_mutex_;
  std::unordered_map<uint32, TcpController*> socket_tcp_map_;

  // Thread pool.
  Executors::FixedThreadPool thread_pool_;
};

}  // namespace net_stack

#endif  // NET_STACK_HOST_
