#include <unistd.h>
#include <string>

#include "Base/Log.h"
#include "Base/Utils.h"
#include "BaseChannel.h"

namespace net_stack {

namespace {

//uint32 error_indexer = 0;

const double kPacketLossRatio = 0;
const double kPacketCorruptRatio = 0;

const uint32 kChannelDelayMicroSeconds = 2 * 10 * 1000;

}  // namespace

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
  usleep(kChannelDelayMicroSeconds);

  std::queue<std::unique_ptr<Packet>> pkts;
  uint32 size = pkts_to_send->size();
  for (uint32 i = 0; i < size; i++) {
    // if (pkts_to_send->front()->tcp_header().seq_num == 10) {
    //   if (error_indexer == 0) {
    //     // Drop the packet.
    //     pkts_to_send->pop();
    //     error_indexer++;
    //     continue;
    //   }
    // }

    // Simulate an unreliable channel: roll a dice and drop the packet.
    if (Utils::RandomFloat() < kPacketLossRatio) {
      printf("*Dropping packet %d\n",
             pkts_to_send->front()->tcp_header().seq_num);
      pkts_to_send->pop();
      continue;
    }

    if (Utils::RandomFloat() < kPacketCorruptRatio) {
      printf("*Corrupting packet %d\n",
             pkts_to_send->front()->tcp_header().seq_num);
      pkts_to_send->front()->set_corrupted(true);
    }

    pkts.push(std::move(pkts_to_send->front()));
    pkts_to_send->pop();
  }

  pkt_buffer_.Push(&pkts);
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
