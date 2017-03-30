#include <thread>

#include "PacketQueue.h"

namespace net_stack {

PacketQueue::PacketQueue() {
  destroy_.store(false);
}

PacketQueue::~PacketQueue() {
  destroy_.store(true);
  cv_.notify_all();
}

bool PacketQueue::Push(std::unique_ptr<Packet> new_ele) {
  std::unique_lock<std::mutex> lock(mutex_);
  packets_.push(std::move(new_ele));
  return true;
}

uint32 PacketQueue::Push(std::queue<std::unique_ptr<Packet>>* pkts) {
  std::unique_lock<std::mutex> lock(mutex_);
  uint32 size = pkts->size();
  for (uint32 i = 0; i < size; i++) {
    packets_.push(std::move(pkts->front()));
    pkts->pop();
  }
  return size;
}

std::unique_ptr<Packet> PacketQueue::DeQueue(bool blocking) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!blocking) {
    if (packets_.empty()) {
      return nullptr;
    }
  } else {
    cv_.wait(lock, [this] { return destroy_.load() || !packets_.empty(); });
  }

  if (destroy_.load()) {
    return nullptr;
  }

  if (packets_.empty()) {
    return nullptr;
  }

  auto re = std::move(packets_.front());
  packets_.pop();
  return re;
}

uint32 PacketQueue::DeQueueAllTo(
    std::queue< std::unique_ptr<Packet> >* receiver_queue, bool blocking) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!blocking) {
    if (packets_.empty()) {
      return 0;
    }
  } else {
    cv_.wait(lock, [this] { return destroy_.load() || !packets_.empty(); });
  }

  if (destroy_.load()) {
    return 0;
  }

  if (packets_.empty()) {
    return 0;
  }

  uint32 total_size = packets_.size();
  receiver_queue->swap(packets_);
  return total_size;
}

uint32 PacketQueue::size() {
  std::unique_lock<std::mutex> lock(mutex_);
  return packets_.size();
}

bool PacketQueue::empty() {
  std::unique_lock<std::mutex> lock(mutex_);
  return packets_.empty();
}

}  // namespace net_stack
