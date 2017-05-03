#include "Base/Log.h"
#include "Base/MacroUtils.h"
#include "SendWindow.h"

namespace net_stack {

namespace {
uint32 kMaxDuplicatedAcksOrigin = 3;
uint32 max_duplicated_acks = kMaxDuplicatedAcksOrigin;
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
  if (free_space() < new_pkt->payload_size() && capacity_ > 0) {
    LogERROR("Can't send new packet - not enough window size");
    return false;
  }

  if (pkts_to_ack_.empty() && new_pkt->tcp_header().seq_num != send_base_) {
    LogERROR("Send queue is empty with send_base = %d, can't send a packet "
             "with seq num = %d", send_base_, new_pkt->tcp_header().seq_num);
    return false;
  }

  if (!pkts_to_ack_.empty()) {
    uint32 last_seq_num = pkts_to_ack_.back().pkt->tcp_header().seq_num;
    uint32 last_pkt_length = pkts_to_ack_.back().pkt->payload_size();
    SANITY_CHECK(last_seq_num + last_pkt_length == send_base_ + size_,
                 "last un-acked pkt mismatch with (send_base + size)");
    if (new_pkt->tcp_header().seq_num != last_seq_num + last_pkt_length) {
      LogERROR("Send queue has last packet (seq_num = %u, size = %u), "
               "expect to send next packet seq_num = %d, "
               "can't send a new packet with seq_num = %u",
               last_seq_num, last_pkt_length,
               last_seq_num + last_pkt_length,
               new_pkt->tcp_header().seq_num);
      return false;
    }
  }

  pkts_to_ack_.emplace(new_pkt);
  size_ += new_pkt->payload_size();
  // Start the RTT stopwatch of this packet.
  pkts_to_ack_.back().rtt_watch_.Start();
  return true;
}

SendWindow::AckResult SendWindow::NewAckedPacket(uint32 ack_num) {
  // Do a sanity check : Is the ack_num within send window?
  // if (capacity_ > 0 &&
  //     send_base_ < send_base_ + capacity_ &&
  //     (ack_num < send_base_ /*|| ack_num > send_base_ + capacity_ */)) {
  //   LogERROR("ack_num %d out of window [%d, %d)",
  //            ack_num, send_base_, send_base_ + capacity_);
  //   return false;
  // }
  // if (capacity_ > 0 &&
  //     send_base_ >= send_base_ + capacity_ &&
  //     (ack_num < send_base_ && ack_num > send_base_ + capacity_)) {
  //   LogERROR("ack_num %d out of overflow window [%d, %d)",
  //            ack_num, send_base_, send_base_ + capacity_);
  //   return false;
  // }

  // Be careful of ack num overflow.
  std::chrono::nanoseconds rtt;
  bool ack_overflow = ack_num < send_base_ && ack_num <= send_base_ + capacity_;
  if (send_base_ < ack_num || ack_overflow) {
    uint32 queue_size = pkts_to_ack_.size();
    uint32 crt_seq_num = 0, crt_pkt_length = 0;
    for (uint32 i = 0; i < queue_size; i++) {
      crt_seq_num = pkts_to_ack_.front().pkt->tcp_header().seq_num;
      crt_pkt_length = pkts_to_ack_.front().pkt->payload_size();
      if (crt_seq_num < ack_num ||
          (ack_overflow && crt_seq_num >= send_base_)) {
        SANITY_CHECK(crt_seq_num + crt_pkt_length <= ack_num ||
                     (ack_overflow && crt_seq_num + crt_pkt_length > ack_num),
                     "ack number %u overlaped with packet %u, size %u",
                     ack_num, crt_seq_num, crt_pkt_length);
        size_ -= pkts_to_ack_.front().pkt->payload_size();
        pkts_to_ack_.front().rtt_watch_.Pause();
        rtt = pkts_to_ack_.front().rtt_watch_.elapsed_time();
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
    max_duplicated_acks = kMaxDuplicatedAcksOrigin;
    bool valid_rtt = !has_retransmitted_pkt_;
    if (pkts_to_ack_.empty()) {
      has_retransmitted_pkt_ = false;
    }
    // If sendwindow has re-transmitted pkt, the rtt is not a valid value to
    // refresh timeout interval.
    return AckResult(true, false, false,
                     valid_rtt? rtt : std::chrono::nanoseconds(0));
  } else if (ack_num == send_base_) {
    duplicated_acks_++;
    if (duplicated_acks_ >= max_duplicated_acks) {
      duplicated_acks_ = 0;
      // Increase duplicated acks torlerance by factor of 1.5 to avoid too many
      // *duplicated* re-transmission.
      max_duplicated_acks *= 1.5;
      return AckResult(false, true, true, std::chrono::nanoseconds(0));
    }
    return AckResult(false, true, false, std::chrono::nanoseconds(0));
  } else {
    return AckResult(false, false, false, std::chrono::nanoseconds(0));
  }
}

std::unique_ptr<Packet> SendWindow::GetBasePakcketToReSend() {
  std::unique_ptr<Packet> pkt;
  if (!pkts_to_ack_.empty()) {
    has_retransmitted_pkt_ = true;
    pkt.reset(pkts_to_ack_.front().pkt->Copy());
  }
  return pkt;
}

uint32 SendWindow::NextSeqNumberToSend() const {
  return send_base_ + size_;
}

uint32 SendWindow::free_space() const {
  if (capacity_ > size_) {
    return capacity_ - size_;
  }

  return 0;
}

}  // namespace net_stack
