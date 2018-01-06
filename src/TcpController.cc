#include <chrono>
#include <climits>
#include <cstdlib>
#include <unistd.h>

#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "Base/Utils.h"
#include "Host.h"
#include "TcpController.h"

namespace net_stack {

namespace {

uint32 kThreadPoolSize = 9;

uint32 kDefaultDataPacketSize = 10;
uint32 kInitialSSThresh = 8 * kDefaultDataPacketSize;

//uint32 kMaxWindowSize = 65536;

uint32 kDefaultWindowBase = 0;
uint32 kDefaultWindowSize = 100;
uint32 kDefaultSocketBufferSize = 100;

std::chrono::nanoseconds kInitialTimeout =
    std::chrono::nanoseconds(500 * 1000 * 1000);
double kRTTExpFactor = 0.125;
double kRTTDevExpFactor = 0.25;

bool kEnableCongestionControl = false;

}

TcpController::TcpController(Host* host,
                             Socket* socket,
                             const TcpControllerKey& tcp_key,
                             const TcpControllerOptions& options) :
    host_(host),
    key_(tcp_key),
    socket_(socket),
    thread_pool_(kThreadPoolSize),
    recv_window_(options.recv_window_base, options.recv_window_size),
    recv_buffer_(options.recv_buffer_size),
    send_buffer_(options.send_buffer_size),
    send_window_(options.send_window_base, options.send_window_size),
    timer_(std::chrono::milliseconds(5 * 100),
           std::bind(&TcpController::TimeoutReTransmitter, this)),
    syn_timer_(std::chrono::seconds(2),
           std::bind(&TcpController::CloseAndDelete, this)),
    fin_timer_(std::chrono::seconds(30),
           std::bind(&TcpController::CloseAndDelete, this)),
    close_timer_(std::chrono::seconds(30),
           std::bind(&TcpController::CloseAndDelete, this)) {
  state_ = CLOSED;
  pipe_state_ = OPEN;

  // Init timeout is 500ms.
  estimated_rtt_ = timeout_interval_ = kInitialTimeout;
  dev_rtt_ = std::chrono::nanoseconds(0);

  cwnd_ = kDefaultDataPacketSize;
  ssthresh_ = kInitialSSThresh;

  timer_.SetRepeat(true);

  thread_pool_.AddTask(
      std::bind(&TcpController::PacketReceiveBufferListener, this));
  thread_pool_.AddTask(
      std::bind(&TcpController::PacketSendBufferListener, this));

  thread_pool_.AddTask(
      std::bind(&TcpController::SendBufferListener, this));
  thread_pool_.AddTask(
      std::bind(&TcpController::ReceiveBufferListener, this));

  thread_pool_.Start();
}

TcpControllerOptions TcpController::GetDefaultOptions() {
  return TcpControllerOptions{kDefaultSocketBufferSize,  // socket send buffer
                              kDefaultWindowBase,  // send window base
                              kDefaultWindowSize,  // send window size
                              kDefaultSocketBufferSize,  // socket recv buffer
                              kDefaultWindowBase,  // recv window base
                              kDefaultWindowSize};  // recv window size
}

TcpController::~TcpController() {
}

void TcpController::TearDown() {
  timer_.Stop();

  state_cv_.notify_all();
  send_window_cv_.notify_all();
  send_buffer_read_cv_.notify_all();
  send_buffer_write_cv_.notify_all();
  recv_buffer_read_cv_.notify_all();

  pkt_recv_buffer_.Stop();
  pkt_send_buffer_.Stop();

  DetachSocket();

  thread_pool_.Stop();
  thread_pool_.AwaitTermination();

  usleep(500);
}

void TcpController::DetachSocket() {
  socket_->tcp_con = nullptr;
}

bool TcpController::TryConnect() {
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    if (state_ != CLOSED) {
      LogERROR("TCP state is not CLOSED, can't Connect");
      return false;
    }
  }

  auto sync_pkt = MakeSyncPacket(send_window_.send_base());

  // This just registers the new packet in send window. Not really sending it.
  {
    std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
    if (!send_window_.SendPacket(sync_pkt)) {
      LogERROR("Failed to send sync packet");
      return false;
    }
  }
  
  // TCP state = SYN_SENT;
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    state_ = SYN_SENT;
  }

  // Actually send the sync packet (1st handshake), and wait in state SYN_SENT
  // for ACK_SYN from server. Before that happens, user can call socket Write()
  // to write data into socket buffer and return success, but no data will be
  // really sent to network until ACK_SYN is received and TCP state transitted
  // to ESTABLISHED.
  SendPacket(std::unique_ptr<Packet>(sync_pkt->Copy()));
  timer_.Restart(CurrentTimeOut());
  syn_timer_.Restart();

  return true;
}

bool TcpController::TryShutDown() {
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    // The connection is already into closing states.
    if (InClosingState()) {
      return true;
    }
  }

  // Send FIN segment. From now on there should be no more data to send. The
  // TCP will transit to FIN_WAIT_1 or LAST_ACK, and InClosingState will return
  // true.
  SendFIN();
  return true;
}

bool TcpController::TryClose() {
  TryShutDown();

  // TCP connection is closed on this side, and no more data is allowed to be
  // received. From now if receive buffer is or becomes non-empty, send back an
  // RST immediately. We implement it by starting a separate thread, which
  // listens to receive buffer and does fake read. It simulates Read() sys call
  // listens app, but just performs at TCP layer. There will be no race on
  // receive buffer, since the socket has been closed and there will be no user
  // thread being able to access this TCP connection.
  CloseConnection();

  // Note the graceful way of TCP disconncting is still working. If the other
  // side continue to send data packets, SendRSTForClosedConnection will send
  // RST back. If the other side received RST, fin_timer will finally timeout
  // and clean up this connection.
  return true;
}

void TcpController::CloseConnection() {
  // Wake up all blocking Read() on this conneciton. They should return -1.
  // Ideally there should not be multi-thread reading the same socket.
  recv_buffer_read_cv_.notify_all();

  thread_pool_.AddTask(
      std::bind(&TcpController::SendRSTForClosedConnection, this));
}

void TcpController::SendRSTForClosedConnection() {
  while (!shutdown_.load()) {
    {
      std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
      recv_buffer_read_cv_.wait(lock, [this] {
        return shutdown_.load() || !recv_buffer_.empty();
      });

      if (shutdown_.load()) {
        break;
      }
      recv_buffer_.Clear();
    }

    // Set TCP state to closed. Now graceful disconnection is disabled. This
    // connection will finally be cleaned up by fin_timer or close_timer.
    {
      std::unique_lock<std::mutex> state_lock(state_mutex_);
      state_ = CLOSED;
    }
    close_timer_.Restart();

    // Receive buffer is not empty after connection has been closed, send RST
    // immediately.
    SendPacket(std::move(MakeRstPacket()));
    // std::queue<std::unique_ptr<Packet>> packets_to_send;
    // packets_to_send.push(std::move(MakeRstPacket()));
    // host_->MultiplexPacketsFromTcp(&packets_to_send);
  }
}

void TcpController::SendFIN() {
  if (shutdown_.load()) {
    return;
  }

  debuginfo("Sending FIN...");

  // Send FIN and transit TCP state.
  std::shared_ptr<Packet> fin_pkt;
  {
    std::unique_lock<std::mutex> lock_send_window(send_window_mutex_);
    fin_pkt = MakeFinPacket(send_window_.NextSeqNumberToSend());
    if (!send_window_.SendPacket(fin_pkt)) {
      LogFATAL("Failed to send FIN segment");
    }
  }

  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    if (state_ == ESTABLISHED) {
      state_ = FIN_WAIT_1;
    } else if (state_ == CLOSE_WAIT) {
      state_ = LAST_ACK;
    } else {
      LogERROR("Can't send FIN at TCP state %s", TcpStateStr(state_).c_str());
      return;
    }
  }

  // Actually send FIN packet.
  SendPacket(std::unique_ptr<Packet>(fin_pkt->Copy()));
  timer_.Restart(CurrentTimeOut());
  fin_timer_.Restart();
}

// This method just enqueue the new packets into this TCP connection's private
// packet receive buffer. It is PacketReceiveBufferListener that monitors this
// queue and handles packets.
void TcpController::ReceiveNewPacket(std::unique_ptr<Packet> packet) {
  pkt_recv_buffer_.Push(std::move(packet));
}

void TcpController::PacketReceiveBufferListener() {
  while (!shutdown_.load()) {
    // Get all new packets.
    std::queue<std::unique_ptr<Packet>> new_packets;
    pkt_recv_buffer_.DeQueueAllTo(&new_packets);

    HandleReceivedPackets(&new_packets);
    SANITY_CHECK(new_packets.empty(),
                 "New packets queue should have been cleared");
  }
}

void TcpController::HandleReceivedPackets(
    std::queue<std::unique_ptr<Packet>>* new_packets) {
  while (!new_packets->empty()) {
    std::unique_ptr<Packet> pkt = std::move(new_packets->front());
    new_packets->pop();

    if (pkt->tcp_header().sync && !pkt->tcp_header().ack) {
      HandleSYN(std::move(pkt));
    } else if (pkt->tcp_header().sync && pkt->tcp_header().ack) {
      HandleACKSYN(std::move(pkt));
    } else if (pkt->tcp_header().fin) {
      HandleFIN(std::move(pkt));
    } else if (pkt->tcp_header().ack) {
      HandleACK(std::move(pkt));
    } else if (pkt->tcp_header().rst) {
      HandleRst();
    } else {
      HandleDataPacket(std::move(pkt));
    }
  }
}

bool TcpController::HandleRst() {
  // Handle RST is easy - Just terminate this TCP connection.
  CloseAndDelete();
  return true;
}

bool TcpController::HandleDataPacket(std::unique_ptr<Packet> pkt) {
  // If this is a zero-size data packet, sender is probing receive window
  // size.
  if (pkt->payload_size() == 0) {
    SendPacket(std::move(MakeAckPacket(recv_window_.recv_base())));
    return true;
  }

  // Handle data packet. Deliver packets to upper layer (socket receive
  // buffer) if avaible, and sends ack packet back to sender.
  auto pair = recv_window_.ReceivePacket(std::move(pkt));
  StreamDataToReceiveBuffer(pair.second);

  SendPacket(std::move(MakeAckPacket(pair.first)));
  return true;
}

bool TcpController::HandleSYN(std::unique_ptr<Packet> pkt) {
  // SYN segment (1st handshake) from client
  //
  // Client is trying to establish connection. Send a SYN_ACK segment back.

  // Receive window's recv base has already been intialized as client's
  // seq_num. Now it should increment by 1 to client_seq_num + 1.
  uint32 client_seq_num = pkt->tcp_header().seq_num;
  auto pair = recv_window_.ReceivePacket(std::move(pkt));
  SANITY_CHECK(pair.first == client_seq_num + 1,
               "Server's recv_base should be client_seq_num + 1 = %u, "
               "but actually it's %u",
               client_seq_num + 1, pair.first);

  // If it is not the first SYN received from client, we alread have a
  // SYN_ACK segment cached in send window. Just re-send it.
  {
    std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
    if (send_window_.size() > 0) {
      SendPacket(send_window_.GetBasePakcketToReSend());
      return true;
    }
  }

  // Create a SYN_ACK packet, with a randomly generated server_seq_num,
  // and ack clients SYN packet with ack_num = client_seq_num + 1.
  uint32 server_seq_num = 0; /* Utils::RandomNumber(); */
  auto sync_ack_pkt = MakeSyncPacket(server_seq_num);
  sync_ack_pkt->mutable_tcp_header()->ack = true;
  sync_ack_pkt->mutable_tcp_header()->ack_num = pair.first;
  {
    // This SYN_ACK packet contains server's receive window size so that
    // client can init its flow control before sending any data packet.
    std::unique_lock<std::mutex> recv_buffer_lock(recv_buffer_mutex_);
    sync_ack_pkt->mutable_tcp_header()
                    ->window_size = recv_buffer_.free_space();
  }

  // SYN_ACK packet is firstly a SYN packet. It needs to be recorded in
  // send window.
  {
    std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
    if (!send_window_.SendPacket(sync_ack_pkt)) {
      LogERROR("Failed to send SYN_ACK packet");
      return false;
    }
  }

  // Really send SYN_ACK segment.
  SendPacket(std::unique_ptr<Packet>(sync_ack_pkt->Copy()));
  timer_.Restart(CurrentTimeOut());
  syn_timer_.Restart();

  // Server's TCP state transit to SYN_RCVD. For now, server can call socket
  // Write() to write data into socket buffer, but no data packet will be
  // actually sent. It must wait for the final ACK (3rd handshake from client)
  // and transit to TCP state ESTABLISHED, before sending any data packet.
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    state_ = SYN_RCVD;
  }

  // Wait for client's ack packet (3rd handshake). It should be a normal
  // ack segment sent from client with ack_num = server_seq_num + 1.
  return true;
}

bool TcpController::HandleACKSYN(std::unique_ptr<Packet> pkt) {
  // SYN_ACK segment (2nd handshake) from server
  //
  // Client handles ack part. After this, client's send base should
  // increment by one, and send windows size is also synced with server's
  // receive buffer size. It is now ready to send data packets.
  std::unique_lock<std::mutex> state_lock(state_mutex_);

  if (state_ == SYN_SENT) {
    // Stop the syn timer.
    syn_timer_.Stop();

    {
      std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
      send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
      if (send_window_.NumPacketsToAck() == 0) {
        timer_.Stop();
      }
      uint32 new_send_window_capacity =
          GetNewSendWindowSize(pkt->tcp_header().window_size);
      debuginfo("set window_size = " +
                std::to_string(new_send_window_capacity));
      send_window_.set_capacity(new_send_window_capacity);
    }

    // Mark client --> server connection is ready.
    state_ = ESTABLISHED;
    debuginfo("Client --> Server connection established ^_^");
    state_cv_.notify_one();
  }
  state_lock.unlock();

  // Client handles sync part. Client should init its receive window base
  // as server's seq_num, and ack this segment (3rd handshake).
  recv_window_.set_recv_base(pkt->tcp_header().seq_num + 1);
  SendPacket(std::move(MakeAckPacket(pkt->tcp_header().seq_num + 1)));
  return true;
}

bool TcpController::HandleFIN(std::unique_ptr<Packet> pkt) {
  // Transit TCP state first. This should be done before delivering this FIN
  // to receive window. Otherwise upper level may get EOF first and call a
  // Close() before TCP transits to CLOST_WAIT state. In this case we'll have
  // both side in FIN_WAIT_1 state.
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  if (state_ == ESTABLISHED) {
    // CLOSE_WAIT state waits for app to call Close() or ShutDown() to send FIN
    // back to the other side.
    debuginfo("Got FIN 1");
    state_ = CLOSE_WAIT;
  } else if (state_ == FIN_WAIT_2) {
    debuginfo("Got FIN 2");
    state_ = TIME_WAIT;

    // Wait for 2 * MSL (typically it should last 1 ~ 2 minutes), and terminate
    // this TCP connection.
    std::thread kill([&] {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      CloseAndDelete();
    });
    kill.detach();
  } else if (state_ == FIN_WAIT_1) {
    // This is simultaneous close case. TCP state should transit to CLOSING.
    debuginfo("Got FIN 1, simultaneous close");
    state_ = CLOSING;
  }
  state_lock.unlock();

  // Deliver this FIN packet to receive window, and maybe it will stream data
  // to receive buffer as EOF.
  auto pair = recv_window_.ReceivePacket(std::move(pkt));
  StreamDataToReceiveBuffer(pair.second);

  // Do ack. Note it may be acking this FIN, or still acking previous data
  // packets, depending on the state of receive window.
  SendPacket(std::move(MakeAckPacket(pair.first)));
  return true;
}

void TcpController::CloseAndDelete() {
  if (shutdown_.load()) {
    return;
  }
  shutdown_.store(true);

  std::thread shut_down([&] {
    TearDown();
    // Notify host to delete this connection and all its resource,
    // after current thread exits. This should be the last thread bound
    // of this connection object.
    std::unique_lock<std::mutex> lock(destroy_mutex_);
    destroy_ = true;
    destroy_cv_.notify_one();
    debuginfo("destroy");
  });
  shut_down.detach();

  // Host will wait for TearDown() to complete.
  std::thread final_clean(
      std::bind(&Host::DeleteTcpConnection, host_, key_));
  final_clean.detach();
}

void TcpController::UpdateRTT(std::chrono::nanoseconds new_rtt) {
  std::unique_lock<std::mutex> rtt_lock(rtt_mutex_);

  estimated_rtt_ = std::chrono::nanoseconds(static_cast<int64>(
      estimated_rtt_.count() * (1 - kRTTExpFactor)
          + new_rtt.count() * kRTTExpFactor));
  dev_rtt_ = std::chrono::nanoseconds(static_cast<int64>(
    dev_rtt_.count() * (1 - kRTTDevExpFactor)
      + std::abs(new_rtt.count() - estimated_rtt_.count()) * kRTTDevExpFactor));
  timeout_interval_ = estimated_rtt_ + 4 * dev_rtt_;
  std::cout << "Update timeout_interval_ = "
            << timeout_interval_.count() / 1000000 << "ms\n";
}

std::chrono::nanoseconds TcpController::CurrentTimeOut() {
  std::unique_lock<std::mutex> rtt_lock(rtt_mutex_);
  return timeout_interval_;
}

void TcpController::UpdateCongestionControl(
    const SendWindow::AckResult& ack_re) {
  std::unique_lock<std::mutex> cc_lock(cc_mutex_);
  switch(cc_state_) {
    case SLOW_START: {
      if (ack_re.ack_refreshed) {
        cwnd_ += kDefaultDataPacketSize;
        if (cwnd_ >= ssthresh_) {
          cc_state_ = CONGESTION_AVOIDANCE;
        }
      } else if (ack_re.re_transmit) {
        ssthresh_ = cwnd_ / 2;
        cwnd_ = ssthresh_ + 2 * kDefaultDataPacketSize;
        cc_state_ = FAST_RECOVERY;
      }
      break;
    }
    case CONGESTION_AVOIDANCE: {
      if (ack_re.ack_refreshed) {
        cwnd_ += kDefaultDataPacketSize * kDefaultDataPacketSize / cwnd_;
      } else if (ack_re.re_transmit) {
        ssthresh_ = cwnd_ / 2;
        cwnd_ = ssthresh_ + 2 * kDefaultDataPacketSize;
        cc_state_ = FAST_RECOVERY;
      }
    }
    case FAST_RECOVERY: {
      if (ack_re.ack_refreshed) {
        cwnd_ = ssthresh_;
        cc_state_ = CONGESTION_AVOIDANCE;
      } else if (ack_re.dup_ack) {
        cwnd_ += kDefaultDataPacketSize;
      }
    }
  }
}

uint32 TcpController::CurrentCWND() {
  std::unique_lock<std::mutex> cc_lock(cc_mutex_);
  return cwnd_;
}

uint32 TcpController::GetNewSendWindowSize(uint32 rwnd) {
  uint32 new_size = rwnd;
  if (kEnableCongestionControl) {
    new_size = std::min(new_size, CurrentCWND());
  }
  return new_size;
}

bool TcpController::HandleACK(std::unique_ptr<Packet> pkt) {
  // ACK segment. Needs to handle TCP state transition.
  std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);

  // Handle ack packet. If detect duplicated ACKs, do a fast re-transmit.
  auto ack_re = send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
  if (ack_re.re_transmit) {
    SendPacket(send_window_.GetBasePakcketToReSend());
  }

  // Update RTT.
  if (ack_re.ack_refreshed && ack_re.rtt.count() > 0) {
    //UpdateRTT(ack_re.rtt);
  }

  // If send window is cleared, stop the timer.
  if (send_window_.NumPacketsToAck() == 0) {
    timer_.Stop();
  } else {
    timer_.Restart(CurrentTimeOut());
  }

  // Update congestion control state.
  //UpdateCongestionControl(ack_re);

  // Flow control - set send window size as receiver indicated.
  uint32 new_send_window_capacity =
      GetNewSendWindowSize(pkt->tcp_header().window_size);
  debuginfo("set window_size = " +
            std::to_string(new_send_window_capacity));
  send_window_.set_capacity(new_send_window_capacity);

  // If send window has free space, notify packet send thread.
  if (send_window_.free_space() > 0 || send_window_.capacity() == 0) {
    send_window_cv_.notify_one();
  }
  bool send_window_empty = (send_window_.NumPacketsToAck() == 0);
  send_window_lock.unlock();

  // Here we need to handle some special acks for TCP handshake and wavebye.
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  if (state_ == SYN_RCVD) {
    // It is 3rd handshake from client, now we can mark server --> client
    // connection is ready to send data.
    state_ = ESTABLISHED;
    syn_timer_.Stop();
    debuginfo("Server --> Client connection established ^_^");
    state_cv_.notify_one();
  } else if (state_ == FIN_WAIT_1 && send_window_empty) {
    // Note this ACK may not be acking the FIN we previously sent. It might
    // just be a regular data packet ACK. So we must check send window is
    // already cleared, since there's no packet in send window after FIN packet.
    // In this way we know for sure this ACK is the FIN ACK we are expecting.
    debuginfo("Got ACK for FIN 1");
    state_ = FIN_WAIT_2;
    fin_timer_.Stop();
  } else if (state_ == LAST_ACK && send_window_empty) {
    debuginfo("Got LAST_ACK");
    // Same as above, need to check send_window_empty. If received last ack,
    // this TCP connection can finally be closed.
    state_ = CLOSED;
    fin_timer_.Stop();
    CloseAndDelete();
  } else if (state_ == CLOSING) {
    state_ = TIME_WAIT;

    // Wait for 2 * MSL (typically it should last 1 ~ 2 minutes), and terminate
    // this TCP connection.
    std::thread kill([&] {
      std::this_thread::sleep_for(std::chrono::seconds(3));
      CloseAndDelete();
    });
    kill.detach();
  }

  return true;
}

void TcpController::StreamDataToReceiveBuffer(
    std::shared_ptr<RecvWindow::RecvWindowNode> received_pkt_nodes) {
  {
    std::unique_lock<std::mutex> recv_buffer_lock(recv_buffer_mutex_);

    // First try to push previously overflowed packets to socket buffer.
    std::unique_lock<std::mutex> overflow_pkts_lock(overflow_pkts_mutex_);
    PushOverflowedPacketsToSocketBuffer();

    // Push received packets to socket receive buffer. If socket buffer is full,
    // push packets into overflowd packets queue.
    std::shared_ptr<RecvWindow::RecvWindowNode> node = received_pkt_nodes;
    while (node) {
      auto pkt = node->pkt;

      // Special segment.
      if (pkt->tcp_header().sync) {
        continue;
      }

      if (pkt->tcp_header().fin) {
        // FIN receivd. No more data should be delivered to upper layer.
        pipe_state_ = EOF_NOT_CONSUMED;
        break;
      }

      if (recv_buffer_.free_space() >= pkt->payload_size()) {
        uint32 writen =
            recv_buffer_.Write(pkt->payload(), pkt->payload_size());
        if (writen <= 0) {
          LogERROR("Socket receive buffer is full, pkt seq = %u is overflowd.",
                   pkt->tcp_header().seq_num);
          overflow_pkts_.push(pkt);
        }
      } else {
        overflow_pkts_.push(pkt);
      }
      node = node->next;
    }
  }
  recv_buffer_read_cv_.notify_one();
}

void TcpController::ReceiveBufferListener() {
  while (!shutdown_.load()) {
    {
      std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
      recv_buffer_write_cv_.wait(lock,
          [this] { return !recv_buffer_.full(); });

      std::unique_lock<std::mutex> overflow_pkts_lock(overflow_pkts_mutex_);
      PushOverflowedPacketsToSocketBuffer();
    }
    recv_buffer_read_cv_.notify_one();
  }
}

void TcpController::PushOverflowedPacketsToSocketBuffer() {
  // Don't lock recv_buffer_mutex! It's already locked in
  // ReceiveBufferListener.
  uint32 overflowed_pkts_size = overflow_pkts_.size();
  for (uint32 i = 0; i < overflowed_pkts_size; i++) {
    auto pkt = overflow_pkts_.front().get();
    if (recv_buffer_.free_space() >= pkt->payload_size()) {
      uint32 writen =
          recv_buffer_.Write(pkt->payload(), pkt->payload_size());
      if (writen > 0) {
        overflow_pkts_.pop();
      } else {
        break;
      }
    } else {
      break;
    }
  }
}

// TODO: add support for non-blocking read.
int32 TcpController::ReadData(byte* buf, int32 size) {
  if (pipe_state_ == EOF_CONSUMED) {
    LogFATAL("There should be no data after EOF.");
  }

  std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
  recv_buffer_read_cv_.wait(lock,
      [this] { return shutdown_.load() ||
                      pipe_state_ == EOF_NOT_CONSUMED ||
                      !recv_buffer_.empty(); });
  if (shutdown_.load()) {
    return -1;
  }

  if (pipe_state_ != EOF_NOT_CONSUMED && recv_buffer_.empty()) {
    return -1;
  }

  // EOF is received, and all data in receive buffer has been read by app,
  // Read() sys call now can return zero.
  if (pipe_state_ == EOF_NOT_CONSUMED && recv_buffer_.empty()) {
    pipe_state_ = EOF_CONSUMED;
    return 0;
  }

  // There is still data in receive buffer. Deliver it to user buffer.
  uint32 readn = recv_buffer_.Read(buf, size);

  if (readn > 0) {
    recv_buffer_write_cv_.notify_one();
  }

  return static_cast<int32>(readn);
}

int32 TcpController::WriteData(const byte* buf, int32 size) {
  {
    // This should not happen. No data should be written after FIN is sent.
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (InClosingState()) {
      return -1;
    }
  }

  uint32 writen = 0;
  {
    std::unique_lock<std::mutex> lock(send_buffer_mutex_);
    // TODO: Non-blocking mode?
    send_buffer_write_cv_.wait(lock, [this] {
        return shutdown_.load() || !send_buffer_.full();
    });
    if (shutdown_.load()) {
      return -1;
    }

    writen = send_buffer_.Write(buf, size);
  }

  send_buffer_read_cv_.notify_one();
  return static_cast<int32>(writen);
}

void TcpController::SendBufferListener() {
  // Wait for TCP connection to be established.
  //
  // Specifically, this is for client to wait in SYN_SENT state , and for
  // server to wait in SYN_RCVD state. They must receive ACK for their SYN,
  // and then transit to state ESTABLISHED respectively, before sending any
  // data packets.
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    state_cv_.wait(state_lock, [&] { return shutdown_.load() ||
                                            !InConnectingState(); });
    if (shutdown_.load()) {
      return;
    }
  }

  while (!shutdown_.load()) {
    // Wait for send window to be not full.
    std::unique_lock<std::mutex> lock_send_window(send_window_mutex_);
    send_window_cv_.wait(lock_send_window,
                         [this] { return shutdown_.load() ||
                                         send_window_.free_space() > 0 ||
                                         send_window_.capacity() <= 0; });
    if (shutdown_.load()) {
      return;
    }
    lock_send_window.unlock();

    // Wait for socket send buffer have data to send.
    std::unique_lock<std::mutex> lock_send_buffer(send_buffer_mutex_);
    send_buffer_read_cv_.wait(lock_send_buffer,
        [this] { return shutdown_.load() || !send_buffer_.empty(); });

    if (shutdown_.load()) {
      return;
    }

    // Lock send window again. Check free space again, because the window size
    // can be reduced by flow control.
    lock_send_window.lock();
    if (send_window_.free_space() <= 0 && send_window_.capacity() > 0) {
      //LogERROR("This should NOT happen. Send window must have space");
      continue;
    }

    // Create data packets and send them out.
    uint32 size_to_send = 0;
    if (send_window_.capacity() == 0) {
      // Send a packet with size = 0, and continue to check if send window has
      // capacity and space. Don't repeatedly send lots of one-byte packets.
      size_to_send = 0;
      // bool restart_timer = (send_window_.NumPacketsToAck() == 0);

      auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                         &send_buffer_, size_to_send);
      // This just mark the new pkt into send window.
      // if (!send_window_.SendPacket(new_data_pkt)) {
      //   continue;
      // }
      // Really send the packet.
      SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
      // if (restart_timer) {
      //   timer_.Restart();
      // }
      //send_buffer_write_cv_.notify_one();
      continue;
    } else {
      size_to_send = Utils::Min(send_window_.free_space(), send_buffer_.size());
    }

    uint32 num_pkts = size_to_send / kDefaultDataPacketSize;
    uint32 last_pkt_size = size_to_send % kDefaultDataPacketSize;
    for (uint32 i = 0; i < num_pkts; i++) {
      bool restart_timer = (send_window_.NumPacketsToAck() == 0);

      auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                         &send_buffer_, kDefaultDataPacketSize);
      // This just mark the new pkt into send window.
      if (!send_window_.SendPacket(new_data_pkt)) {
        continue;
      }
      // Really send the packet.
      SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
      if (restart_timer) {
        timer_.Restart(CurrentTimeOut());
      }
    }

    if (last_pkt_size > 0) {
      bool restart_timer = (send_window_.size() == 0);
      auto new_data_pkt = MakeDataPacket(send_window_.NextSeqNumberToSend(),
                                         &send_buffer_, last_pkt_size);
      if (!send_window_.SendPacket(new_data_pkt)) {
        continue;
      }
      SendPacket(std::unique_ptr<Packet>(new_data_pkt->Copy()));
      if (restart_timer) {
        timer_.Restart(CurrentTimeOut());
      }
    }
    // Notify new data can be written to send buffer from user space.
    send_buffer_write_cv_.notify_one();
  }
}

void TcpController::SendPacket(std::unique_ptr<Packet> pkt) {
  if (!pkt) {
    return;
  }

  debuginfo(pkt->DebugString());

  pkt_send_buffer_.Push(std::move(pkt));
}

void TcpController::PacketSendBufferListener() {
  while (!shutdown_.load()) {
    std::queue<std::unique_ptr<Packet>> packets_to_send;
    pkt_send_buffer_.DeQueueAllTo(&packets_to_send);

    // Deliver packets to host buffer.
    host_->MultiplexPacketsFromTcp(&packets_to_send);
  }
}

void TcpController::TimeoutReTransmitter() {
  // Current timer expired. Re-transmit the oldest packet in send window. Note
  // timer will be automatically restarted.
  std::unique_lock<std::mutex> lock(send_window_mutex_);
  if (send_window_.NumPacketsToAck() > 0) {
    SendPacket(send_window_.GetBasePakcketToReSend());
  }

  std::unique_lock<std::mutex> cc_lock(cc_mutex_);
  ssthresh_ = cwnd_ / 2;
  cwnd_ = kDefaultDataPacketSize;
  cc_state_ = SLOW_START;

  // TODO: Double the timeout of timer, for congestion control?
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, const byte* data, uint32 size) {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.seq_num = seq_num;
  tcp_header.ack = false;

  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header, data, size));
  return pkt;
}

std::shared_ptr<Packet> TcpController::MakeDataPacket(
    uint32 seq_num, Utility::BufferInterface* data_buffer, uint32 size) {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.seq_num = seq_num;
  tcp_header.ack = false;

  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  pkt->InjectPayloadFromBuffer(data_buffer, size);
  return pkt;
}

std::unique_ptr<Packet> TcpController::MakeAckPacket(uint32 ack_num) {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.ack = true;
  tcp_header.ack_num = ack_num;

  {
    std::unique_lock<std::mutex> recv_buffer_lock(recv_buffer_mutex_);
    tcp_header.window_size = recv_buffer_.free_space();
  }

  std::unique_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  return pkt;
}

std::shared_ptr<Packet> TcpController::MakeSyncPacket(uint32 seq_num) {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.sync = true;
  tcp_header.seq_num = seq_num;

  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  pkt->set_payload_size(1);
  return pkt;
}

std::shared_ptr<Packet> TcpController::MakeFinPacket(uint32 seq_num) {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.fin = true;
  tcp_header.seq_num = seq_num;

  std::shared_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  pkt->set_payload_size(1);
  return pkt;
}

std::unique_ptr<Packet> TcpController::MakeRstPacket() {
  IPHeader ip_header;
  ip_header.source_ip = key_.source_ip;
  ip_header.dest_ip = key_.dest_ip;

  TcpHeader tcp_header;
  tcp_header.source_port = key_.source_port;
  tcp_header.dest_port = key_.dest_port;
  tcp_header.rst = true;

  std::unique_ptr<Packet> pkt(new Packet(ip_header, tcp_header));
  return pkt;
}

void TcpController::debuginfo(const std::string& msg) {
  LogINFO((host_->hostname() + ": " + msg).c_str());
}

void TcpController::WaitForReadyToDestroy() {
  // Wait for destroy signal.
  std::unique_lock<std::mutex> lock(destroy_mutex_);
  destroy_cv_.wait(lock, [&] { return destroy_; });
}

bool TcpController::InConnectingState() {
  return state_ == CLOSED || state_ == SYN_SENT || state_ == SYN_RCVD;
}

bool TcpController::InClosingState() {
  // These are state after either side sends FIN.
  //
  // Note CLOSE_WAIT state is not included. It still allows sending data.
  // A closing state could only be triggered by SendFIN.
  return state_ == FIN_WAIT_1 || state_ == FIN_WAIT_2 || state_ == TIME_WAIT ||
         state_ == CLOSING || state_ == LAST_ACK;
}

std::string TcpController::TcpStateStr(TCP_STATE state) {
  switch (state_) {
    case CLOSED: return "CLOSED";
    case SYN_SENT: return "SYN_SENT";
    case ESTABLISHED: return "ESTABLISHED";
    case LISTEN: return "LISTEN";
    case SYN_RCVD: return "SYN_RCVD";
    case FIN_WAIT_1: return "FIN_WAIT_1";
    case FIN_WAIT_2: return "FIN_WAIT_2";
    case TIME_WAIT: return "TIME_WAIT";
    case CLOSE_WAIT: return "CLOSE_WAIT";
    case LAST_ACK: return "LAST_ACK";
    default: return "UNKNOWN_TCP_STATE";
  }
}

}  // namespace net_stack
