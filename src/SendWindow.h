#ifndef NET_STACK_SEND_WINDOW_
#define NET_STACK_SEND_WINDOW_

#include <memory>
#include <queue>
#include <utility>

#include "Base/BaseTypes.h"
#include "Packet.h"
#include "TcpWindow.h"
#include "Utility/StopWatch.h"

namespace net_stack {

class SendWindow : public TcpWindow {
 public:
  struct SendWindowNode {
    SendWindowNode(std::shared_ptr<Packet> packet) : pkt(packet) {}

    std::shared_ptr<Packet> pkt;
    Utility::StopWatch rtt_watch_;
    uint32 re_transmit_count_ = 0;
  };

  SendWindow(uint32 send_base);
  SendWindow(uint32 send_base, uint32 capacity);

  // Put a newly sent packet into window. Return true on success.
  bool SendPacket(std::shared_ptr<Packet> pkt);

  // Get a newly acked packet. Check if we can move forward send base.
  // Return value true indicates overly duplicated ACKs, and we need do a fast
  // re-transmit.
  struct AckResult {
    AckResult(bool ack_refreshed_, bool dup_ack_, bool re_transmit_,
              std::chrono::nanoseconds rtt_) :
        ack_refreshed(ack_refreshed_),
        dup_ack(dup_ack_),
        re_transmit(re_transmit_),
        rtt(rtt_) {
    }

    // Send base is updated.
    bool ack_refreshed = false;
    bool dup_ack = false;
    bool re_transmit = false;
    std::chrono::nanoseconds rtt;  // nanoseconds
  };

  AckResult NewAckedPacket(uint32 ack_num);

  DEFINE_ACCESSOR(capacity, uint32);
  DEFINE_ACCESSOR(send_base, uint32);

  uint32 size() const { return size_; }
  uint32 free_space() const;

  uint32 NumPacketsToAck() const { return pkts_to_ack_.size(); }

  uint32 NextSeqNumberToSend() const;

  std::unique_ptr<Packet> GetBasePakcketToReSend();

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
  std::queue<SendWindowNode> pkts_to_ack_;

  uint32 last_acked_num_ = 0;
  uint32 duplicated_acks_ = 0;

  bool has_retransmitted_pkt_ = false;
};

}  // namespace net_stack

#endif  // NET_STACK_SEND_WINDOW_
