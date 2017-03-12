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
  payload_ = new char[data.size()];
  memcpy(payload_, data.c_str(), data.size());
}

// It does NOT take ownership of the payload buffer.
Packet::Packet(const IPHeader& ip_header, const TcpHeader& tcp_header,
               const char* data, int size) :
    ip_header_(ip_header),
    tcp_header_(tcp_header) {
  InjectPayload(data, size);
}

Packet::~Packet() {
  if (payload_) {
    delete[] payload_;
  }
}

void Packet::InjectPayload(const char* data, int size) {
  if (payload_) {
    delete[] payload_;
  }
  payload_ = new char[size];
  memcpy(payload_, data, size);
}

}  // namespace net_stack