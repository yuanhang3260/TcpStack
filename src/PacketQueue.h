#ifndef NET_STACK_PACKET_QUEUE_
#define NET_STACK_PACKET_QUEUE_

#include <memory>
#include <queue>

#include "Base/BaseTypes.h"
#include "Packet.h"

namespace net_stack {

// This class wrapps a queue of Packet. This class is not thread-safe.
class PacketQueue {
 public:
  PacketQueue() = default;

  void Push(std::unique_ptr<Packet> ele);

  std::unique_ptr<Packet> DeQueue();
  int DeQueueAllTo(std::queue< std::unique_ptr<Packet> >* receiver_queue);

  uint32 size() const;
  bool empty() const;

 private:
  std::queue<std::unique_ptr<Packet>> packets_;
};

}  // namespace net_stack

#endif  // NET_STACK_PACKET_QUEUE_