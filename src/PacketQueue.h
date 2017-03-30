#ifndef NET_STACK_PACKET_QUEUE_
#define NET_STACK_PACKET_QUEUE_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>

#include "Base/BaseTypes.h"
#include "Packet.h"

namespace net_stack {

// This class wrapps a queue of Packet. This class is thread-safe. It supports
// blocking and non-blocking reading.
class PacketQueue {
 public:
  PacketQueue();
  ~PacketQueue();

  bool Push(std::unique_ptr<Packet> ele);
  uint32 Push(std::queue<std::unique_ptr<Packet>>* pkts);

  // In non-blocking mode, if queue is empty, imediately return nullptr.
  std::unique_ptr<Packet> DeQueue(bool blocking = true);
  // In non-blocking mode, if queue is empty, immediately return 0.
  uint32 DeQueueAllTo(std::queue< std::unique_ptr<Packet> >* receiver_queue,
                      bool blocking = true);

  uint32 size();
  bool empty();

 private:
  std::queue<std::unique_ptr<Packet>> packets_;
  std::mutex mutex_;
  std::condition_variable cv_;

  std::atomic_bool destroy_;
};

}  // namespace net_stack

#endif  // NET_STACK_PACKET_QUEUE_