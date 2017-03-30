#include <string>

#include "Base/Log.h"
#include "BaseChannel.h"

namespace net_stack {

BaseChannel::BaseChannel(ReceiverCallBack receiver_callback) :
    receiver_callback_(std::move(receiver_callback)) {
  stop_.store(true, std::memory_order_relaxed);
}

BaseChannel::~BaseChannel() {
  stop_.store(true, std::memory_order_relaxed);
  pkt_buffer_.Stop();

  if (listner_.joinable()) {
    listner_.join();
  }
}

void BaseChannel::Start() {
  stop_.store(false, std::memory_order_relaxed);
  listner_ = std::thread(std::bind(&BaseChannel::WaitingForPackets, this));
}

void BaseChannel::Send(std::unique_ptr<Packet> packet) {
  pkt_buffer_.Push(std::move(packet));
}

void BaseChannel::Send(std::queue<std::unique_ptr<Packet>>* pkts_to_send) {
  pkt_buffer_.Push(pkts_to_send);
}

void BaseChannel::Send(Packet* packet) {
  pkt_buffer_.Push(std::unique_ptr<Packet>(packet));
}

void BaseChannel::WaitingForPackets() {
  while (!stop_.load(std::memory_order_relaxed)) {
    std::queue<std::unique_ptr<Packet>> new_packets;
    auto re = pkt_buffer_.DeQueueAllTo(&new_packets);
    if (re == 0) {
      continue;
    }

    std::unique_lock<std::mutex> lock(cb_mutex_);
    if (receiver_callback_) {
      receiver_callback_(&new_packets);
    }
  }
}

void BaseChannel::RegisterReceiverCallback(ReceiverCallBack receiver_callback) {
  std::unique_lock<std::mutex> lock(cb_mutex_);
  receiver_callback_ = std::move(receiver_callback);
}

}  // namespace net_stack
