#ifndef NET_STACK_TCP_CONTROLLER_
#define NET_STACK_TCP_CONTROLLER_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Strings/Utils.h"
#include "Utility/InfiniteBuffer.h"
#include "Utility/RingBuffer.h"
#include "Utility/ThreadPool.h"
#include "Utility/Timer.h"

#include "BaseChannel.h"
#include "Kernel.h"
#include "PacketQueue.h"
#include "RecvWindow.h"
#include "SendWindow.h"

namespace net_stack {

class Host;

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
    CLOSING,
    CLOSE_WAIT,
    LAST_ACK,
  };

  TcpController(Host* host, Socket* socket,
                const TcpControllerKey& tcp_key,
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
  bool TryClose();

  // Terminate this connection. It stops all threads of this connection object.
  void TearDown();

  // If set true, this TCP connection is responsible for releasing port in
  // clean up.
  void SetReleasePort(bool set);

  const TcpControllerKey& key() const;
  TCP_STATE state() const;

 private:
  // These methods serve uplink packet/data delivery (receive data).
  void PacketReceiveBufferListener();
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
  void SendBufferListener();
  void SendPacket(std::unique_ptr<Packet> pkt);
  void PacketSendBufferListener();

  // Socket receive buffer listner. It waits for socket receive buffer to
  // become non-empty, and push overflowed packets into it.
  void ReceiveBufferListener();
  void PushOverflowedPacketsToSocketBuffer();

  void SendFIN();

  void CloseAndDelete();
  void DetachSocket();

  // Timeout callback.
  void TimeoutReTransmitter();

  // Send prober packet.
  void ProbeWindowSize();

  std::shared_ptr<Packet> MakeDataPacket(
      uint32 seq_num, const byte* data, uint32 size);
  std::shared_ptr<Packet> MakeDataPacket(
    uint32 seq_num, Utility::BufferInterface* data_buffer, uint32 size);
  
  std::unique_ptr<Packet> MakeAckPacket(uint32 ack_num);

  std::shared_ptr<Packet> MakeSyncPacket(uint32 seq_num);

  std::shared_ptr<Packet> MakeFinPacket(uint32 seq_num);

  std::unique_ptr<Packet> MakeRstPacket();

  void CloseConnection();
  void SendRSTForClosedConnection();

  std::string TcpStateStr(TCP_STATE state);

  bool InConnectingState();
  bool InClosingState();

  void UpdateRTT(std::chrono::nanoseconds new_rtt);
  std::chrono::nanoseconds CurrentTimeOut();

  void UpdateCongestionControl(const SendWindow::AckResult& ack_re);
  uint32 CurrentCWND();

  uint32 GetNewSendWindowSize(uint32 rwnd);

  std::string hostname() const;

  Host* host_ = nullptr;
  TcpControllerKey key_;
  Socket* socket_ = nullptr;

  Executors::FixedThreadPool thread_pool_;

  // *************************** Receive Pipeline *************************** //
  // Packet receive buffer. This is the low-level queue to buffer received
  // packets delivered from host (namely layer 2).
  PacketQueue pkt_recv_buffer_;

  // Recv window. No mutex is needed. Only PacketReceiveBufferListner thread
  // could modify it.
  RecvWindow recv_window_;

  // Socket receive buffer. Note pipe_state_ is also protected by its mutex,
  // since we use it to indicate receive buffer has received EOF.
  Utility::RingBuffer recv_buffer_;
  std::mutex recv_buffer_mutex_;
  std::condition_variable recv_buffer_read_cv_;
  std::condition_variable recv_buffer_write_cv_;

  enum PipeState {
    OPEN,
    EOF_NOT_CONSUMED,
    EOF_CONSUMED,
  };
  PipeState pipe_state_;

  // This is a temporary queue to store overflowed packets when socket receive
  // buffer is full. This is for a corner case of flow control. When receive
  // window size is 0, sender will continue sending packets with size 1 byte.
  std::queue<std::shared_ptr<Packet>> overflow_pkts_;
  std::mutex overflow_pkts_mutex_;

  // *************************** Send Pipeline ****************************** //
  // Socket send buffer.
  Utility::RingBuffer send_buffer_;
  std::mutex send_buffer_mutex_;
  std::condition_variable send_buffer_read_cv_;  // send buffer has data
  std::condition_variable send_buffer_write_cv_; // send buffer has space
  std::condition_variable send_buffer_empty_cv_; // send buffer is empty

  // Send window.
  SendWindow send_window_;
  std::mutex send_window_mutex_;
  std::condition_variable send_window_cv_;

  // Packet send buffer. This is the low-level queue to buffer packets to send
  // to host (namely layer 2).
  PacketQueue pkt_send_buffer_;

  // ***************************** Timers *********************************** //
  // This is the TCP sliding window timer.
  Utility::Timer timer_;

  // Other timers which we use in connection management.
  Utility::Timer syn_timer_;
  Utility::Timer fin_timer_;
  Utility::Timer close_timer_;
  Utility::Timer prober_timer_;  // send window size prober timer


  // **************************** TCP state ********************************* //
  // TCP state.
  TCP_STATE state_ = CLOSED;
  std::mutex state_mutex_;
  std::condition_variable state_cv_;

  // RTT calculation.
  std::chrono::nanoseconds estimated_rtt_;
  std::chrono::nanoseconds dev_rtt_;  // deviation
  std::chrono::nanoseconds timeout_interval_;
  std::mutex rtt_mutex_;

  // Congestion control.
  enum CongestionControlState {
    SLOW_START,
    CONGESTION_AVOIDANCE,
    FAST_RECOVERY,
  };
  CongestionControlState cc_state_ = SLOW_START;
  uint32 cwnd_;
  uint32 ssthresh_;
  std::mutex cc_mutex_;

  std::atomic_bool shutdown_;

  bool release_port_ = false;
};

}  // namespace net_stack

#endif  // NET_STACK_TCP_CONTROLLER_
