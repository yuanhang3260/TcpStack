#ifndef NET_STACK_TCP_CONTROLLER_
#define NET_STACK_TCP_CONTROLLER_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "BaseChannel.h"
#include "PacketQueue.h"
#include "RecvWindow.h"
#include "SendWindow.h"
#include "Utility/InfiniteBuffer.h"
#include "Utility/ThreadPool.h"

namespace net_stack {

// TCP uses <source_ip, source_port, dest_ip, dest_port> as unique indentifier.
struct TcpControllerKey {
  std::string source_ip;
  int source_port;
  std::string dest_ip;
  int dest_port;

  bool operator==(const TcpControllerKey &other) const { 
    return (source_ip == other.source_ip && source_port == other.source_port &&
            dest_ip == other.dest_ip && dest_port == other.dest_port);
  }

  bool operator<(const TcpControllerKey &other) const {
    if (source_ip != other.source_ip) {
      return source_ip < other.source_ip;
    }
    if (source_port != other.source_port) {
      return source_port < other.source_port;
    }
    if (dest_ip != other.dest_ip) {
      return dest_ip < other.dest_ip;
    }
    if (dest_port != other.dest_port) {
      return dest_port < other.dest_port;
    }

    return false;
  }
};

class Host;

struct TcpControllerOptions {
  TcpControllerKey key;

  uint32 send_buffer_size = 0;
  uint32 send_window_base = 0;
  uint32 send_window_size = 0;

  uint32 recv_buffer_size = 0;
  uint32 recv_window_base = 0;
  uint32 recv_window_size = 0;
};

// TcpController is completely event-driven. It has monitors for all 
class TcpController {
 public:
  TcpController(Host* host, const TcpControllerOptions& options);

  static TcpControllerOptions GetDefaultOptions();

  // Host delivers new packet to this TCP connection.
  void ReceiveNewPacket(std::unique_ptr<Packet> packet);

  // This is the actual streaming API for application layer to call. 
  int32 ReadData(byte* buf, int32 size);

 private:
  // These methods serve uplink packet/data delivery (receive data).
  void PacketReceiveBufferListner();
  void HandleReceivedPackets(std::queue<std::unique_ptr<Packet>>* new_packets);
  std::unique_ptr<Packet> MakeAckPacket(uint32 ack_num);
  void StreamDataToReceiveBuffer(
      std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes);

  // These method serve down-link packet/data delivery (send data).
  void SocketSendBufferListener();

  Host* host_ = nullptr;
  Executors::FixedThreadPool thread_pool_;

  TcpControllerKey key_;

  // Socket send buffer.
  Utility::InfiniteBuffer send_buffer_;
  std::mutex send_buffer_mutex_;
  std::condition_variable send_buffer_cv_;
  // Send window.
  SendWindow send_window_;

  // Socket receive buffer.
  Utility::InfiniteBuffer recv_buffer_;
  std::mutex recv_buffer_mutex_;
  std::condition_variable recv_buffer_cv_;
  // Recv window.
  RecvWindow recv_window_;

  // Packet receive buffer. This is the low-level queue to buffer received
  // packets delivered from host (namely layer 2).
  PacketQueue pkt_recv_buffer_;
  std::mutex pkt_recv_buffer_mutex_;
  std::condition_variable pkt_recv_buffer_cv_;
};

}  // namespace net_stack


namespace std {
template <>
struct hash<net_stack::TcpControllerKey> {
  size_t operator() (const net_stack::TcpControllerKey& socket_id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(socket_id.source_ip) ^
              (int_hasher(socket_id.source_port) << 1)) >> 1) ^
             (((str_hasher(socket_id.dest_ip) << 2) ^
               (int_hasher(socket_id.dest_port) << 2)) >> 2);
  }
};
}  // namespace

#endif  // NET_STACK_TCP_CONTROLLER_
