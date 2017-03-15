#include "Base/Log.h"
#include "RecvWindow.h"

namespace net_stack {

RecvWindow::RecvWindow(uint32 recv_base) :
    recv_base_(recv_base),
    capacity_(TcpWindow::kDefaultWindowSize),
    head_(new RecvWindowNode) {
}

RecvWindow::RecvWindow(uint32 recv_base, uint32 capacity) :
    recv_base_(recv_base),
    capacity_(capacity),
    head_(new RecvWindowNode) {
}

std::pair<uint32, std::shared_ptr<RecvWindow::RecvWindowNode>>
RecvWindow::ReceivePacket(std::unique_ptr<Packet> new_pkt) {
  // Create a new node and insert it to the proper position of queue.
  std::shared_ptr<RecvWindowNode> new_node(new RecvWindowNode());
  new_node->pkt.reset(new_pkt.release());

  uint32 new_pkt_seq = new_node->pkt->tcp_header().seq_num;
  uint32 new_pkt_length = new_node->pkt->payload_size();
  std::shared_ptr<RecvWindowNode> node = head_;
  while (node->next) {
    uint32 next_pkt_seq = node->next->pkt->tcp_header().seq_num;
    uint32 next_pkt_length = node->next->pkt->payload_size();
    if (new_pkt_seq > next_pkt_seq) {
      SANITY_CHECK(new_pkt_seq >= next_pkt_seq + next_pkt_length,
                   "Packet size ranges overlap")
      node = node->next;
    } else {
      break;
    }
  }
  new_node->next = node->next;
  if (new_node->next) {
    new_node->next->prev = new_node;
    SANITY_CHECK(
        new_pkt_seq +  new_pkt_length <=
            new_node->next->pkt->tcp_header().seq_num,
        "Packet size ranges overlap");
  }
  new_node->prev = node;
  node->next = new_node;

  // If new packet's seq number is expected as recv_base, update recv_base,
  // and check following out-of-order packets to deliver to upper layer.
  if (new_node->pkt->tcp_header().seq_num == recv_base_) {
    SANITY_CHECK(new_node == head_->next,
                 "new node is in-order, must have been inserted to queue head");
    std::shared_ptr<RecvWindowNode> node = new_node;
    while (node && node->next) {
      uint32 crt_pkt_seq = node->pkt->tcp_header().seq_num;
      uint32 crt_pkt_length = node->pkt->payload_size();
      uint32 next_pkt_seq = node->next->pkt->tcp_header().seq_num;
      if (crt_pkt_seq + crt_pkt_length == next_pkt_seq) {  // overflow is fine
        node = node->next;
      } else {
        break;
      }
    }
    head_->next = node->next;
    if (node->next) {
      node->next->prev = head_;
    }
    new_node->prev = nullptr;
    node->next = nullptr;
    recv_base_ = node->pkt->tcp_header().seq_num + node->pkt->payload_size();
    return std::make_pair(recv_base_, new_node);
  }
  else {
    // It's an out-of-order packet. Re-ack recv_base.
    return std::make_pair(recv_base_, nullptr);
  }
}

}  // namespace net_stack
