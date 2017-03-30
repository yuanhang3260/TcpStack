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
#include "Utility/Timer.h"

namespace net_stack {

class Host;

// TCP uses <source_ip, source_port, dest_ip, dest_port> as unique indentifier.
struct TcpControllerKey {
  std::string source_ip;
  uint32 source_port;
  std::string dest_ip;
  uint32 dest_port;

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

  std::string DebugString() const {
    return "{" + source_ip + ", " + std::to_string(source_port) + ", " +
           dest_ip + ", " + std::to_string(dest_port) + "}";
  }
};

struct TcpControllerOptions {
  uint32 send_buffer_size;
  uint32 send_window_base;
  uint32 send_window_size;

  uint32 recv_buffer_size;
  uint32 recv_window_base;
  uint32 recv_window_size;
};

// TcpController is completely event-driven. It has monitors for all 
class TcpController {
 public:
  TcpController(Host* host, const TcpControllerKey& tcp_id, uint32 socket_fd,
                const TcpControllerOptions& options);
  ~TcpController();

  static TcpControllerOptions GetDefaultOptions();

  // Host delivers new packet to this TCP connection.
  void ReceiveNewPacket(std::unique_ptr<Packet> packet);

  // This is the actual streaming API for application layer to call. 
  int32 ReadData(byte* buf, int32 size);
  int32 WriteData(const byte* buf, int32 size);

 private:
  // These methods serve uplink packet/data delivery (receive data).
  void PacketReceiveBufferListner();
  void HandleReceivedPackets(std::queue<std::unique_ptr<Packet>>* new_packets);
  void StreamDataToReceiveBuffer(
      std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes);

  // These method serve down-link packet/data delivery (send data).
  void SocketSendBufferListener();
  void SendPacket(std::unique_ptr<Packet> pkt);
  void PacketSendBufferListner();

  // Timeout callback.
  void TimeoutReTransmitter();

  std::shared_ptr<Packet> MakeDataPacket(
      uint32 seq_num, const byte* data, uint32 size);
  std::shared_ptr<Packet> MakeDataPacket(
    uint32 seq_num, Utility::BufferInterface* data_buffer, uint32 size);
  
  std::unique_ptr<Packet> MakeAckPacket(uint32 ack_num);

  Host* host_ = nullptr;
  TcpControllerKey key_;
  uint32 socket_fd_;

  Executors::FixedThreadPool thread_pool_;

  // ************** Receive Pipeline ************** //
  // Packet receive buffer. This is the low-level queue to buffer received
  // packets delivered from host (namely layer 2).
  PacketQueue pkt_recv_buffer_;
  // Recv window. No mutex needed. Only PacketReceiveBufferListner thread
  // use it.
  RecvWindow recv_window_;
  // Socket receive buffer.
  Utility::InfiniteBuffer recv_buffer_;
  std::mutex recv_buffer_mutex_;
  std::condition_variable recv_buffer_cv_;

  // *************** Send Pipeline **************** //
  // Socket send buffer.
  Utility::InfiniteBuffer send_buffer_;
  std::mutex send_buffer_mutex_;
  std::condition_variable send_buffer_cv_;
  std::condition_variable send_buffer_write_cv_;
  // Send window.
  SendWindow send_window_;
  std::mutex send_window_mutex_;
  std::condition_variable send_window_cv_;
  // Packet send buffer. This is the low-level queue to buffer packets to send
  // to host (namely layer 2).
  PacketQueue pkt_send_buffer_;

  // Timer
  Utility::Timer timer_;
};

}  // namespace net_stack


namespace std {
template <>
struct hash<net_stack::TcpControllerKey> {
  size_t operator() (const net_stack::TcpControllerKey& tcp_id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(tcp_id.source_ip) ^
              (int_hasher(tcp_id.source_port) << 1)) >> 1) ^
             (((str_hasher(tcp_id.dest_ip) << 2) ^
               (int_hasher(tcp_id.dest_port) << 2)) >> 2);
  }
};
}  // namespace

#endif  // NET_STACK_TCP_CONTROLLER_
