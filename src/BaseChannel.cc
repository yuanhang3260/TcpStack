#include <string>

#include "BaseChannel.h"

namespace net_stack {

BaseChannel::BaseChannel(ReceiverCallBack receiver_callback) :
    receiver_callback_(receiver_callback) {
}

BaseChannel::~BaseChannel() {
  {
    std::unique_lock<std::mutex> lock(pkt_buffer_mutex_);
    stop_ = true;
  }
  pkt_buffer_cv_.notify_one();
  if (listner_.joinable()) {
    listner_.join();
  }
}

void BaseChannel::Start() {
  {
    std::unique_lock<std::mutex> lock(pkt_buffer_mutex_);
    if (!stop_) {
      return;
    }
    stop_ = false;
  }
  listner_ = std::thread(std::bind(&BaseChannel::WaitingForPackets, this));
}

void BaseChannel::Send(std::unique_ptr<Packet> packet) {
  {
    std::unique_lock<std::mutex> lock(pkt_buffer_mutex_);
    pkt_buffer_.Push(std::move(packet));
  }
  pkt_buffer_cv_.notify_one();
}

void BaseChannel::Send(Packet* packet) {
  {
    std::unique_lock<std::mutex> lock(pkt_buffer_mutex_);
    pkt_buffer_.Push(std::unique_ptr<Packet>(packet));
  }
  pkt_buffer_cv_.notify_one();
}

void BaseChannel::RegisterReceiverCallback(ReceiverCallBack receiver_callback) {
  std::unique_lock<std::mutex> lock(cb_mutex_);
  receiver_callback_ = receiver_callback;
}

void BaseChannel::WaitingForPackets() {
  while (1) {
    std::queue<std::unique_ptr<Packet>> new_packets_;
    {
      std::unique_lock<std::mutex> lock(pkt_buffer_mutex_);
      pkt_buffer_cv_.wait(lock,
                          [this] { return !pkt_buffer_.empty() || stop_; });
      if (stop_) {
        break;
      }
      pkt_buffer_.DeQueueAllTo(&new_packets_);
    }

    {
      std::unique_lock<std::mutex> lock(cb_mutex_);
      if (receiver_callback_) {
        receiver_callback_(new_packets_);
      }
    }
  }
}

}  // namespace net_stack
