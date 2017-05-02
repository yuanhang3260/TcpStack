#ifndef NET_STACK_TCP_CONTROLLER_
#define NET_STACK_TCP_CONTROLLER_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "BaseChannel.h"
#include "Common.h"
#include "PacketQueue.h"
#include "RecvWindow.h"
#include "SendWindow.h"
#include "Strings/Utils.h"
#include "Utility/RingBuffer.h"
#include "Utility/RingBuffer.h"
#include "Utility/ThreadPool.h"
#include "Utility/Timer.h"

namespace net_stack {

class Host;
class Socket;

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
    return Strings::StrCat("{", source_ip, ":", std::to_string(source_port),
                           ", ", dest_ip, ":", std::to_string(dest_port), "}");
  }
};

// Local listener key. It only bundles {local ip, local_port} as key.
struct LocalLayerThreeKey {
  std::string local_ip;
  uint32 local_port;

  bool operator==(const LocalLayerThreeKey &other) const { 
    return local_ip == other.local_ip && local_port == other.local_port;
  }

  bool operator<(const LocalLayerThreeKey &other) const {
    if (local_ip != other.local_ip) {
      return local_ip < other.local_ip;
    }
    if (local_port != other.local_port) {
      return local_port < other.local_port;
    }

    return false;
  }

  std::string DebugString() const {
    return Strings::StrCat("{", local_ip, ", ",
                           std::to_string(local_port), "}");
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
  enum TCP_STATE {
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    LISTEN,
    SYN_RCVD,
    FIN_WAIT_1,
    FIN_WAIT_2,
    TIME_WAIT,
    CLOSE_WAIT,
    LAST_ACK,
  };

  TcpController(Host* host, const TcpControllerKey& tcp_id,
                std::shared_ptr<net_stack::Socket> socket_,
                const TcpControllerOptions& options);
  ~TcpController();

  static TcpControllerOptions GetDefaultOptions();

  // Host delivers new packet to this TCP connection.
  void ReceiveNewPacket(std::unique_ptr<Packet> packet);

  // This is the actual streaming API for application layer to call. 
  int32 ReadData(byte* buf, int32 size);
  int32 WriteData(const byte* buf, int32 size);

  // Connect to remote host. It sends a SYN segment.
  bool TryConnect();

  // Shut down a TCP connection. It sends a FIN segment.
  bool TryShutDown();

  // Terminate this connection. It stops all threads of this connection object.
  void TearDown();

  // This is called by host to wait for this connection object can be deleted.
  void WaitForReadyToDestroy();

  int32 socket_fd() const;

 private:
  // These methods serve uplink packet/data delivery (receive data).
  void PacketReceiveBufferListner();
  void HandleReceivedPackets(std::queue<std::unique_ptr<Packet>>* new_packets);
  bool HandleACK(std::unique_ptr<Packet> pkt);
  bool HandleSYN(std::unique_ptr<Packet> pkt);
  bool HandleACKSYN(std::unique_ptr<Packet> pkt);
  bool HandleFIN(std::unique_ptr<Packet> pkt);
  bool HandleRst();
  bool HandleDataPacket(std::unique_ptr<Packet> pkt);
  void StreamDataToReceiveBuffer(
      std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes);

  // These method serve down-link packet/data delivery (send data).
  void SocketSendBufferListener();
  void SendPacket(std::unique_ptr<Packet> pkt);
  void PacketSendBufferListener();

  // Socket receive buffer listner. It waits for socket receive buffer to
  // become non-empty, and push overflowed packets into it.
  void SocketReceiveBufferListener();
  void PushOverflowedPacketsToSocketBuffer();

  void SendFIN();

  void CloseAndDelete();

  // Timeout callback.
  void TimeoutReTransmitter();

  std::shared_ptr<Packet> MakeDataPacket(
      uint32 seq_num, const byte* data, uint32 size);
  std::shared_ptr<Packet> MakeDataPacket(
    uint32 seq_num, Utility::BufferInterface* data_buffer, uint32 size);
  
  std::unique_ptr<Packet> MakeAckPacket(uint32 ack_num);

  std::shared_ptr<Packet> MakeSyncPacket(uint32 seq_num);

  std::shared_ptr<Packet> MakeFinPacket(uint32 seq_num);

  std::unique_ptr<Packet> MakeRstPacket();

  std::string TcpStateStr(TCP_STATE state);

  bool InConnectingState();
  bool InClosingState();

  void UpdateRTT(std::chrono::nanoseconds new_rtt);
  std::chrono::nanoseconds CurrentTimeOut();

  void debuginfo(const std::string& msg);

  Host* host_ = nullptr;
  TcpControllerKey key_;
  std::shared_ptr<net_stack::Socket> socket_;

  Executors::FixedThreadPool thread_pool_;

  // ************** Receive Pipeline ************** //
  // Packet receive buffer. This is the low-level queue to buffer received
  // packets delivered from host (namely layer 2).
  PacketQueue pkt_recv_buffer_;

  // Recv window. No mutex needed. Only PacketReceiveBufferListner thread
  // use it.
  RecvWindow recv_window_;
  std::atomic_bool fin_received_;
  std::atomic_uint fin_seq_;

  // Socket receive buffer.
  Utility::RingBuffer recv_buffer_;
  std::mutex recv_buffer_mutex_;
  std::condition_variable recv_buffer_read_cv_;
  std::condition_variable recv_buffer_write_cv_;

  enum PipeState {
    OPEN,
    EOF_READ,
    EOF_NOT_READ,
  };
  std::atomic<PipeState> pipe_state_;

  // This is a temporary queue to store overflowed packets when socket receive
  // buffer is full. This is for a corner case of flow control. When receive
  // window size is 0, sender will continue sending packets with size 1 byte.
  std::queue<std::shared_ptr<Packet>> overflow_pkts_;
  std::mutex overflow_pkts_mutex_;

  // *************** Send Pipeline **************** //
  // Socket send buffer.
  Utility::RingBuffer send_buffer_;
  std::mutex send_buffer_mutex_;
  std::condition_variable send_buffer_data_cv_;  // send buffer has data
  std::condition_variable send_buffer_write_cv_; // send buffer has space
  std::condition_variable send_buffer_empty_cv_; // send buffer is empty

  // Send window.
  SendWindow send_window_;
  std::mutex send_window_mutex_;
  std::condition_variable send_window_cv_;

  // Packet send buffer. This is the low-level queue to buffer packets to send
  // to host (namely layer 2).
  PacketQueue pkt_send_buffer_;

  // This is the TCP sliding window timer.
  Utility::Timer timer_;

  // Other timers which we use in connection management.
  Utility::Timer syn_timer_;
  Utility::Timer fin_timer_;

  // TCP state.
  TCP_STATE state_ = CLOSED;
  std::mutex state_mutex_;
  std::condition_variable state_cv_;

  // RTT calculation.
  std::chrono::nanoseconds estimated_rtt_;
  std::chrono::nanoseconds dev_rtt_;  // deviation
  std::chrono::nanoseconds timeout_interval_;
  std::mutex rtt_mutex_;

  std::atomic_bool shutdown_;

  bool destroy_ = false;
  std::mutex destroy_mutex_;
  std::condition_variable destroy_cv_;
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

template <>
struct hash<net_stack::LocalLayerThreeKey> {
  size_t operator() (const net_stack::LocalLayerThreeKey& id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(id.local_ip) ^
              (int_hasher(id.local_port) << 1)) >> 1);
  }
};

}  // namespace

#endif  // NET_STACK_TCP_CONTROLLER_
