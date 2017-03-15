#ifndef NET_STACK_RECV_WINDOW_
#define NET_STACK_RECV_WINDOW_

#include <memory>
#include <utility>

#include "Base/BaseTypes.h"
#include "Packet.h"
#include "TcpWindow.h"

namespace net_stack {

class RecvWindow : public TcpWindow {
 public:
  // Receive window is a doubly-linked list, because it needs to handle out of
  // order packets.
  struct RecvWindowNode {
    std::shared_ptr<Packet> pkt;
    std::shared_ptr<RecvWindowNode> prev;
    std::shared_ptr<RecvWindowNode> next;
  };

  RecvWindow(uint32 recv_base);
  RecvWindow(uint32 recv_base, uint32 capacity);

  // Receive a new data packet, and returns a pair of
  // <ack number, list of packets to deliver to upper layer>
  std::pair<uint32, std::shared_ptr<RecvWindowNode>>
  ReceivePacket(std::unique_ptr<Packet> new_pkt);

  uint32 capacity() const { return capacity_; }

 private:
  // Receive base. This is the next seq number expected to receive.
  uint32 recv_base_ = 0;

  // This is the total window size.
  uint32 capacity_ = 0;

  // Note head is a dummy head.
  std::shared_ptr<RecvWindowNode> head_;
};

}  // namespace net_stack

#endif  // NET_STACK_RECV_WINDOW_
