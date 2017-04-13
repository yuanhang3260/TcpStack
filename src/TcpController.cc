#include <chrono>

#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "Base/Utils.h"
#include "Host.h"
#include "TcpController.h"

namespace net_stack {

namespace {
uint32 kThreadPoolSize = 4;

uint32 kDefaultDataPacketSize = 10;

//uint32 kMaxWindowSize = 65536;

uint32 kDefaultWindowBase = 0;
uint32 kDefaultWindowSize = 100;
uint32 kDefaultSocketBufferSize = 100;
}

TcpController::TcpController(Host* host,
                             const TcpControllerKey& tcp_key,
                             int32 socket_fd,
                             const TcpControllerOptions& options) :
    host_(host),
    key_(tcp_key),
    socket_fd_(socket_fd),
    thread_pool_(kThreadPoolSize),
    recv_window_(options.recv_window_base, options.recv_window_size),
    recv_buffer_(options.recv_buffer_size),
    send_buffer_(options.send_buffer_size),
    send_window_(options.send_window_base, options.send_window_size),
    timer_(std::chrono::milliseconds(5 * 100),
           std::bind(&TcpController::TimeoutReTransmitter, this)) {
  state_ = CLOSED;
  socket_status_.store(OPEN);

  timer_.SetRepeat(true);

  thread_pool_.AddTask(
      std::bind(&TcpController::PacketReceiveBufferListner, this));
  thread_pool_.AddTask(
      std::bind(&TcpController::SocketSendBufferListener, this));
  thread_pool_.AddTask(
      std::bind(&TcpController::PacketSendBufferListener, this));
  // thread_pool_.AddTask(
  //     std::bind(&TcpController::SocketReceiveBufferListener, this));
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

void TcpController::ShutDown() {
  if (shutdown_.load()) {
    return;
  }

  shutdown_.store(true);
  send_window_cv_.notify_all();
  send_buffer_cv_.notify_all();

  pkt_recv_buffer_.Stop();
  pkt_send_buffer_.Stop();

  thread_pool_.Stop();
  thread_pool_.AwaitTermination();

  LogINFO("TcpConnection %s destroyed ^_^", key_.DebugString().c_str());
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

  {
    std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
    if (!send_window_.SendPacket(sync_pkt)) {
      LogERROR("Failed to send sync packet");
      return false;
    }
  }

  // Send the sync packet (1st handshake), and wait for remote host's
  // SYNC_ACK segment (2nd handshake).
  //
  // Here we simply wait for TCP state transitted to ESTABLISHED.
  SendPacket(std::unique_ptr<Packet>(sync_pkt->Copy()));
  timer_.Restart();
  
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  state_ = SYN_SENT;
  state_cv_.wait(state_lock, [&] { return state_ == ESTABLISHED; });

  return true;
}

bool TcpController::TryClose() {
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    // The connection is already into closing states.
    if (state_ == CLOSE_WAIT || state_ == LAST_ACK || state_ == CLOSED) {
      return true;
    }
    if (state_ != ESTABLISHED) {
      LogERROR("TCP state is not ESTABLISHED, can't Close");
      return false;
    }
  }

  std::shared_ptr<Packet> fin_pkt;
  {
    std::unique_lock<std::mutex> lock_send_window(send_window_mutex_);
    fin_pkt = MakeFinPacket(send_window_.NextSeqNumberToSend());
    if (!send_window_.SendPacket(fin_pkt)) {
      LogFATAL("Failed to send FIN segment");
      return false;
    }
  }

  // Need to set TCP state here, before really sending the FIN segment.
  // Otherwise it can over-write following states (FIN_WAIT_2, TIME_WAIT).
  {
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    state_ = FIN_WAIT_1;
  }
  SendPacket(std::unique_ptr<Packet>(fin_pkt->Copy()));
  timer_.Restart();

  // Wait for TCP status transitted to TIME_WAIT. This state can last a
  // relatively long time (~1 minute), but Close() can safely return in this
  // TCP state. When TCP state finally transitted to CLOSED, all connection
  // resource (port, buffer, etc) will be released by host system.
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  state_cv_.wait(state_lock, [&] { return state_ == TIME_WAIT; });

  return true;
}

// This method just enqueue the new packets into this TCP connection's private
// packet receive buffer. It is PacketReceiveBufferListner that monitors this
// queue and handles packets.
void TcpController::ReceiveNewPacket(std::unique_ptr<Packet> packet) {
  pkt_recv_buffer_.Push(std::move(packet));
}

void TcpController::PacketReceiveBufferListner() {
  while (true) {
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

    // SYNC segment (1st handshake) from client
    //
    // Client is trying to establish connection. Send a SYNC_ACK segment back.
    if (pkt->tcp_header().sync && !pkt->tcp_header().ack) {
      // Receive window's recv base has already been intialized as client's
      // seq_num. Now it should increment by 1 to client_seq_num + 1.
      uint32 client_seq_num = pkt->tcp_header().seq_num;
      auto pair = recv_window_.ReceivePacket(std::move(pkt));
      SANITY_CHECK(pair.first == client_seq_num + 1,
                   "Server's recv_base should be client_seq_num + 1 = %u, "
                   "but actually it's %u",
                   client_seq_num + 1, pair.first);

      // If it is not the first SYNC received from client, we alread have a
      // SYNC_ACK segment cached in send window. Just re-send it.
      {
        std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
        if (send_window_.size() > 0) {
          SendPacket(send_window_.BasePakcketWaitingForAck());
          continue;
        }
      }

      // Create a SYNC_ACK packet, with a randomly generated server_seq_num,
      // and ack clients SYNC packet with ack_num = client_seq_num + 1.
      uint32 server_seq_num = 0; /* Utils::RandomNumber(); */
      auto sync_ack_pkt = MakeSyncPacket(server_seq_num);
      sync_ack_pkt->mutable_tcp_header()->ack = true;
      sync_ack_pkt->mutable_tcp_header()->ack_num = pair.first;
      {
        // This SYNC_ACK packet contains server's receive window size so that
        // client can init its flow control before sending any data packet.
        std::unique_lock<std::mutex> recv_buffer_lock(recv_buffer_mutex_);
        sync_ack_pkt->mutable_tcp_header()
                        ->window_size = recv_buffer_.free_space();
      }

      // SYNC_ACK packet is firstly a SYNC packet. It needs to be recorded in
      // send window.
      {
        std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
        if (!send_window_.SendPacket(sync_ack_pkt)) {
          LogERROR("Failed to send SYNC_ACK packet");
          continue;
        }
      }

      // Really send SYNC_ACK segment.
      SendPacket(std::unique_ptr<Packet>(sync_ack_pkt->Copy()));
      timer_.Restart();

      // Server's TCP state transition.
      {
        std::unique_lock<std::mutex> state_lock(state_mutex_);
        state_ = SYN_RCVD;
      }

      // Wait for client's ack packet (3rd handshake). It should be a normal
      // ack segment sent from client with ack_num = server_seq_num + 1.

    } else if (pkt->tcp_header().sync && pkt->tcp_header().ack) {
      // SYNC_ACK segment (2nd handshake) from server
      //
      // Client handles ack part. After this, client's send base should
      // increment by one, and send windows size is also synced with server's
      // receive buffer size. It is now ready to send data packets.
      std::unique_lock<std::mutex> state_lock(state_mutex_);
      if (state_ == SYN_SENT) {
        {
          std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);
          send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
          if (send_window_.NumPacketsToAck() == 0) {
            timer_.Stop();
          }
          debuginfo("set window_size = " +
                    std::to_string(pkt->tcp_header().window_size));
          send_window_.set_capacity(pkt->tcp_header().window_size);
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

    } else if (pkt->tcp_header().fin) {
      // FIN segment. Ack this segment, and transit TCP state.

      // Deliver this FIN packet to upper level to notify blocking Read().
      auto pair = recv_window_.ReceivePacket(std::move(pkt));
      StreamDataToReceiveBuffer(pair.second);

      // Ack this FIN, and transit TCP state.
      std::unique_lock<std::mutex> state_lock(state_mutex_);
      SendPacket(std::move(MakeAckPacket(pair.first)));

      if (state_ == ESTABLISHED) {
        state_ = CLOSE_WAIT;
      } else if (state_ == FIN_WAIT_2) {
        // Notify the thread waiting on Close().
        state_ = TIME_WAIT;
        state_cv_.notify_one();

        // Wait for 2 seconds (in reality TIME_WAIT should last 1 ~ 2 minutes),
        // and destroy this TCP connection.
        std::thread shut_down([&] {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          ShutDown();
          // Notify host to delete this connection and all its resource,
          // after current thread exits. This should be the last thread bound
          // of this connection object.
          std::unique_lock<std::mutex> lock(destroy_mutex_);
          std::notify_all_at_thread_exit(destroy_cv_, std::move(lock));
          destroy_ = true;
        });
        shut_down.detach();

        std::thread final_clean(
            std::bind(&Host::DeleteTcpConnection, host_, key_));
        final_clean.detach();
      }

    } else if (pkt->tcp_header().ack) {
      std::unique_lock<std::mutex> send_window_lock(send_window_mutex_);

      // Handle ack packet. If detect duplicated ACKs, do a fast re-transmit.
      bool re_transmit = send_window_.NewAckedPacket(pkt->tcp_header().ack_num);
      if (re_transmit) {
        SendPacket(send_window_.BasePakcketWaitingForAck());
      }
      // If send window is cleared, stop the timer.
      if (send_window_.NumPacketsToAck() == 0) {
        timer_.Stop();
      } else {
        timer_.Restart();
      }

      // Flow control - set send window size as receiver indicated.
      debuginfo("set window_size = " +
                std::to_string(pkt->tcp_header().window_size));
      send_window_.set_capacity(pkt->tcp_header().window_size);

      // If send window has free space, notify packet send thread.
      if (send_window_.free_space() > 0 || send_window_.capacity() == 0) {
        send_window_cv_.notify_one();
      }
      send_window_lock.unlock();

      // Here we need to handle some special acks for TCP handshake and wavebye.
      bool close_wait_done = false;
      {
        std::unique_lock<std::mutex> state_lock(state_mutex_);
        if (state_ == SYN_RCVD) {
          // If server is waiting for client's 3rd handshake ack segment, now
          // we can mark server --> client connection is ready.
          state_ = ESTABLISHED;
          debuginfo("Server --> Client connection established ^_^");
        } else if (state_ == FIN_WAIT_1) {
          // Received ACK for FIN_WAIT_1, transit to FIN_WAIT_2.
          state_ = FIN_WAIT_2;
        } else if (state_ == CLOSE_WAIT &&
                   send_window_.NumPacketsToAck() == 0) {
          // All data has been sent. Now it's okay to send second FIN and wait
          // for final ack.
          close_wait_done = true;
        } else if (state_ == LAST_ACK) {
          // Received last ack. This TCP connection can finally be closed.
          state_ = CLOSED;

          std::thread shut_down([&] {
            ShutDown();
            // Notify host to delete this connection and all its resource,
            // after current thread exits. This should be the last thread bound
            // of this connection object.
            std::unique_lock<std::mutex> lock(destroy_mutex_);
            std::notify_all_at_thread_exit(destroy_cv_, std::move(lock));
            destroy_ = true;
          });
          shut_down.detach();

          std::thread final_clean(
              std::bind(&Host::DeleteTcpConnection, host_, key_));
          final_clean.detach();
        }
      }

      if (close_wait_done) {
        std::shared_ptr<Packet> fin_pkt;
        {
          std::unique_lock<std::mutex> lock_send_window(send_window_mutex_);
          fin_pkt = MakeFinPacket(send_window_.NextSeqNumberToSend());
          if (!send_window_.SendPacket(fin_pkt)) {
            LogFATAL("Failed to send FIN segment");
            continue;
          }
        }
        {
          std::unique_lock<std::mutex> state_lock(state_mutex_);
          state_ = LAST_ACK;
        }
        SendPacket(std::unique_ptr<Packet>(fin_pkt->Copy()));
        timer_.Restart();
      }

    } else {
      if (pkt->payload_size() == 0) {
        // This is a zero-size data packet. Sender is probing receive window
        // size.
        SendPacket(std::move(MakeAckPacket(recv_window_.recv_base())));
        continue;
      }

      // Handle data packet. Deliver packets to upper layer (socket receive
      // buffer) if avaible, and sends ack packet back to sender.
      auto pair = recv_window_.ReceivePacket(std::move(pkt));
      StreamDataToReceiveBuffer(pair.second);
      SendPacket(std::move(MakeAckPacket(pair.first)));
    }
  }
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
        socket_status_.store(EOF_NOT_READ);
        break;
      }

      if (recv_buffer_.free_space() >= pkt->payload_size()) {
        uint32 writen =
            recv_buffer_.Write(pkt->payload(), pkt->payload_size());
        if (writen <= 0) {
          LogFATAL("Socket receive buffer is full, pkt seq = %u is dropped.",
                   pkt->tcp_header().seq_num);
          // LogFATAL("hehe");

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

void TcpController::SocketReceiveBufferListener() {
  while (true) {
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
  // {
  //   std::unique_lock<std::mutex> lock(state_mutex_);
  //   if (state_ != ESTABLISHED) {
  //     LogERROR("TCP connection not established, abort ReadData");
  //     return -1;
  //   }
  // }

  if (socket_status_.load() == EOF_READ) {
    LogERROR("Broken pipe %d", socket_fd_);
    return -1;
  }

  std::unique_lock<std::mutex> lock(recv_buffer_mutex_);
  recv_buffer_read_cv_.wait(lock,
      [this] { return socket_status_.load() == EOF_NOT_READ ||
                      !recv_buffer_.empty(); });

  // Socket is closed by the other side.
  if (socket_status_.load() == EOF_NOT_READ && recv_buffer_.empty()) {
    socket_status_.store(EOF_READ);
    return 0;
  }

  // Copy data to user buffer.
  // TODO: replace with RingBuffer to check flow control.
  uint32 readn = recv_buffer_.Read(buf, size);

  if (readn > 0) {
    recv_buffer_write_cv_.notify_one();
  }

  // Cast should be safe. We'll never have a receive buffer as big as 2^31
  return static_cast<int32>(readn);
}

int32 TcpController::WriteData(const byte* buf, int32 size) {
  {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != ESTABLISHED) {
      LogERROR("TCP state not in ESTABLISHED, cannot send data");
      return -1;
    }
  }

  uint32 writen = 0;
  {
    std::unique_lock<std::mutex> lock(send_buffer_mutex_);
    // TODO: Non-blocking mode?
    send_buffer_write_cv_.wait(lock,
        [this] { return !send_buffer_.full(); });
    writen = send_buffer_.Write(buf, size);
  }

  send_buffer_cv_.notify_one();
  return static_cast<int32>(writen);
}

void TcpController::SocketSendBufferListener() {
  while (true) {
    // User called Close() and TCP state transitted to FIN_WAIT_1, no more data
    // should sent from now. This thread can safely exit.
    {
      std::unique_lock<std::mutex> lock_send_window(state_mutex_);
      if (state_ == FIN_WAIT_1) {
        break;
      }
    }

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

    // Wait for socket send buffer to have data to send.
    std::unique_lock<std::mutex> lock_send_buffer(send_buffer_mutex_);
    send_buffer_cv_.wait(lock_send_buffer,
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
        timer_.Restart();
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
        timer_.Restart();
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
  while (true) {
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
    SendPacket(send_window_.BasePakcketWaitingForAck());
  }

  // TODO: Double the timeout of timer, for congestion control.
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

void TcpController::debuginfo(const std::string& msg) {
  LogINFO((host_->hostname() + ": " + msg).c_str());
}

void TcpController::WaitForReadyToDestroy() {
  // Wait for destroy signal.
  std::unique_lock<std::mutex> lock(destroy_mutex_);
  destroy_cv_.wait(lock, [&] { return destroy_; });
}

}  // namespace net_stack
