#include <thread>

#include "Utility/CleanUp.h"
#include "PacketQueue.h"

namespace net_stack {

PacketQueue::PacketQueue() {
  destroy_.store(false);
  num_readers_ = 0;
}

PacketQueue::~PacketQueue() {
  Stop();
}

void PacketQueue::Stop() {
  if (destroy_.load()) {
    return;
  }

  destroy_.store(true);
  cv_.notify_all();

  // Wait for all reading client threads to return.
  {
    std::unique_lock<std::mutex> lock(num_readers_mutex_);
    num_readers_cv_.wait(lock, [this] { return num_readers_ <= 0; });
  }
}

bool PacketQueue::Push(std::unique_ptr<Packet> new_ele) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    packets_.push(std::move(new_ele));
  }
  cv_.notify_one();
  return true;
}

uint32 PacketQueue::Push(std::queue<std::unique_ptr<Packet>>* pkts) {
  uint32 size = pkts->size();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (uint32 i = 0; i < size; i++) {
      packets_.push(std::move(pkts->front()));
      pkts->pop();
    }
  }

  cv_.notify_one();
  return size;
}

std::unique_ptr<Packet> PacketQueue::DeQueue(bool blocking) {
  IncReaders();
  auto cleanup = Utility::CleanUp(std::bind(&PacketQueue::DecReaders, this));

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
  IncReaders();
  auto cleanup = Utility::CleanUp(std::bind(&PacketQueue::DecReaders, this));

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

void PacketQueue::IncReaders() {
  std::unique_lock<std::mutex> lock(num_readers_mutex_);
  num_readers_++;
}

void PacketQueue::DecReaders() {
  bool last_reader = false;
  {
    std::unique_lock<std::mutex> lock(num_readers_mutex_);
    last_reader = (num_readers_ == 1);
    num_readers_--;
  }

  if (destroy_.load() && last_reader) {
    num_readers_cv_.notify_one();
  }
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
