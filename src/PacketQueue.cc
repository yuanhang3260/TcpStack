#include "PacketQueue.h"

namespace net_stack {

void PacketQueue::Push(std::unique_ptr<Packet> new_ele) {
  packets_.push(std::move(new_ele));
}

uint32 PacketQueue::Push(std::queue<std::unique_ptr<Packet>>* pkts) {
  uint32 size = pkts->size();
  for (uint32 i = 0; i < size; i++) {
    packets_.push(std::move(pkts->front()));
    pkts->pop();
  }
  return size;
}

std::unique_ptr<Packet> PacketQueue::DeQueue() {
  auto re = std::move(packets_.front());
  packets_.pop();
  return re;
}

uint32 PacketQueue::DeQueueAllTo(
    std::queue< std::unique_ptr<Packet> >* receiver_queue) {
  uint32 total_size = packets_.size();
  receiver_queue->swap(packets_);
  return total_size;
}

uint32 PacketQueue::size() const {
  return packets_.size();
}

bool PacketQueue::empty() const {
  return packets_.empty();
}

}  // namespace net_stack
