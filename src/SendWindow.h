#ifndef NET_STACK_SEND_WINDOW_
#define NET_STACK_SEND_WINDOW_

#include <memory>
#include <queue>
#include <utility>

#include "Base/BaseTypes.h"
#include "Packet.h"
#include "TcpWindow.h"

namespace net_stack {

class SendWindow : public TcpWindow {
 public:
  SendWindow(uint32 send_base);
  SendWindow(uint32 send_base, uint32 capacity);

  // Put a newly sent packet into window. Return true on success.
  bool SendPacket(std::shared_ptr<Packet> pkt);

  // Get a newly acked packet. Check if we can move forward send base.
  // Return value true indicates overly duplicated ACKs, and we need do a fast
  // re-transmit.
  bool NewAckedPacket(uint32 ack_num);

  uint32 capacity() const { return capacity_; }
  uint32 size() const { return size_; }
  uint32 free_space() const { return capacity_ - size_; }
  uint32 send_base() const { return send_base_; }

  uint32 NumPacketsToAck() const { return pkts_to_ack_.size(); }

  uint32 NextSeqNumberToSend() const;

  std::unique_ptr<Packet> BasePakcketWaitingForAck() const;

  void Reset();

 private:
  // Send base. This is least un-acked packet seq number.
  uint32 send_base_ = 0;

  // This is the total window size.
  uint32 capacity_ = 0;

  // Currently used space.
  uint32 size_ = 0;

  // We use shared_ptr to save pakcets waiting for ack, to indicate they're not
  // "movable", because we may need re-transmission.
  std::queue<std::shared_ptr<Packet>> pkts_to_ack_;

  uint32 last_acked_num_ = 0;
  uint32 duplicated_acks_ = 0;
};

}  // namespace net_stack

#endif  // NET_STACK_SEND_WINDOW_
