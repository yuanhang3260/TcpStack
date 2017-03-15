#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "SendWindow.h"

namespace net_stack {

namespace {
uint32 kMaxDuplicatedAcks = 2;
}

SendWindow::SendWindow(uint32 send_base) :
    send_base_(send_base),
    capacity_(TcpWindow::kDefaultWindowSize) {
}

SendWindow::SendWindow(uint32 send_base, uint32 capacity) :
    send_base_(send_base),
    capacity_(capacity) {
}

bool SendWindow::SendPacket(std::shared_ptr<Packet> new_pkt) {
  if (free_space() < new_pkt->payload_size()) {
    LogERROR("Can't send new packet - not enough window size");
    return false;
  }

  if (pkts_to_ack_.empty() && new_pkt->tcp_header().seq_num != send_base_) {
    LogERROR("Send queue is empty with send_base = %d, can't send a packet "
             "with seq num = %d", send_base_, new_pkt->tcp_header().seq_num);
    return false;
  }

  if (!pkts_to_ack_.empty()) {
    uint32 last_seq_num = pkts_to_ack_.back()->tcp_header().seq_num;
    uint32 last_pkt_length = pkts_to_ack_.back()->payload_size();
    if (new_pkt->tcp_header().seq_num != last_seq_num + last_pkt_length) {
      LogERROR("Send queue has last packet %u size %u, "
               "expect to send next packet seq num = %d, "
               "can't send a new packet with seq num = %u",
               last_seq_num, last_pkt_length,
               last_seq_num + last_pkt_length,
               new_pkt->tcp_header().seq_num);
      return false;
    }
  }

  pkts_to_ack_.push(new_pkt);
  size_ += new_pkt->payload_size();
  return true;
}

bool SendWindow::NewAckedPacket(uint32 ack_num) {
  // Do a sanity check : Is the ack_num within send window?
  if (send_base_ < send_base_ + capacity_ &&
      (ack_num < send_base_ || ack_num > send_base_ + capacity_)) {
    LogFATAL("ack_num %d out of window [%d, %d)",
             ack_num, send_base_, send_base_ + capacity_);
    return true;
  }
  if (send_base_ >= send_base_ + capacity_ &&
      (ack_num < send_base_ && ack_num > send_base_ + capacity_)) {
    LogFATAL("ack_num %d out of overflow window [%d, %d)",
             ack_num, send_base_, send_base_ + capacity_);
    return true;
  }

  // Be careful of ack num overflow.
  bool ack_overflow = ack_num < send_base_ && ack_num <= send_base_ + capacity_;
  if (send_base_ < ack_num || ack_overflow) {
    uint32 queue_size = pkts_to_ack_.size();
    uint32 crt_seq_num = 0, crt_pkt_length = 0;
    for (uint32 i = 0; i < queue_size; i++) {
      crt_seq_num = pkts_to_ack_.front()->tcp_header().seq_num;
      crt_pkt_length = pkts_to_ack_.front()->payload_size();
      if (crt_seq_num < ack_num ||
          (ack_overflow && crt_seq_num >= send_base_)) {
        SANITY_CHECK(crt_seq_num + crt_pkt_length <= ack_num ||
                     (ack_overflow && crt_seq_num + crt_pkt_length > ack_num),
                     "ack number %u overlaped with packet %u, size %u",
                     ack_num, crt_seq_num, crt_pkt_length);
        size_ -= pkts_to_ack_.front()->payload_size();
        pkts_to_ack_.pop();
      } else {
        break;
      }
    }

    uint32 expected_next_ack = pkts_to_ack_.empty() ?
        crt_seq_num + crt_pkt_length : crt_seq_num;
    SANITY_CHECK(ack_num == expected_next_ack,
                 "Ack num %u mismatch with expected ack %u",
                 ack_num, expected_next_ack);

    send_base_ = ack_num;
    last_acked_num_ = ack_num;
    duplicated_acks_ = 0;
    return true;
  } else if (ack_num == send_base_) {
    duplicated_acks_++;
    if (duplicated_acks_ >= kMaxDuplicatedAcks) {
      duplicated_acks_ = 0;
      return false;
    }
    return true;
  } else {
    return true;
  }
}

}  // namespace net_stack