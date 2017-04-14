#include <stdlib.h>
#include <string.h>

#include "Packet.h"

namespace net_stack {

Packet::Packet(const IPHeader& ip_header, const TcpHeader& tcp_header) :
    ip_header_(ip_header),
    tcp_header_(tcp_header) {
}

Packet::Packet(const IPHeader& ip_header, const TcpHeader& tcp_header,
               const std::string& data) :
    ip_header_(ip_header),
    tcp_header_(tcp_header) {
  payload_ = new byte[data.size()];
  memcpy(payload_, data.c_str(), data.size());
  payload_size_ = data.size();
}

// It does NOT take ownership of the payload buffer.
Packet::Packet(const IPHeader& ip_header, const TcpHeader& tcp_header,
               const byte* data, int size) :
    ip_header_(ip_header),
    tcp_header_(tcp_header) {
  InjectPayload(data, size);
}

Packet::~Packet() {
  if (payload_) {
    delete[] payload_;
  }
}

uint32 Packet::InjectPayload(const byte* data, int size) {
  if (!data || size <= 0) {
    return 0;
  }

  if (payload_) {
    delete[] payload_;
  }
  payload_ = new byte[size];
  memcpy(payload_, data, size);
  payload_size_ = size;
  return size;
}

uint32 Packet::InjectPayloadFromBuffer(
    Utility::BufferInterface* src_buffer, uint32 size) {
  if (size == 0) {
    payload_size_ = 0;
    return 0;
  }

  if (payload_) {
    delete[] payload_;
  }
  payload_ = new byte[size];

  payload_size_ = src_buffer->Read(payload_, size);
  return payload_size_;
}

Packet* Packet::Copy() const {
  Packet* copy = new Packet(ip_header_, tcp_header_);
  copy->payload_size_ = payload_size_;

  copy->InjectPayload(payload_, payload_size_);
  return copy;
}

std::string Packet::DebugString() const {
  std::string debug_msg = tcp_header_.ack ?
      "ack " + std::to_string(tcp_header_.ack_num): "";

  if (tcp_header_.sync) {
    debug_msg += ", sync";
  } else if (tcp_header_.fin) {
    debug_msg += ", fin";
  } 

  if (payload_size_ > 0) {
    debug_msg += (", seq " + std::to_string(tcp_header_.seq_num));
  } else if (!tcp_header_.ack) {
    // If not a ack packet, and payload size is zero, it's a receive window
    // size prober.
    debug_msg += "probing window size";
  }

  return debug_msg;
}

}  // namespace net_stack