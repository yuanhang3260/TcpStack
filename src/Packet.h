#ifndef NET_STACK_PACKET_
#define NET_STACK_PACKET_

#include <string>

#include "IPHeader.h"
#include "TCPHeader.h"

namespace net_stack {

// This class is a abstraction of IP packet, which contains TCP/IP header and
// application payload. We use this as the low-level network packet.
//
// This class is not thread-safe.
class Packet {
 public:
  Packet(const IPHeader& ip_header, const TCPHeader& tcp_header);
  Packet(const IPHeader& ip_header, const TCPHeader& tcp_header,
         const std::string& payload);
  // It does NOT take ownership of the payload buffer.
  Packet(const IPHeader& ip_header, const TCPHeader& tcp_header,
         const char* payload, int size);

  ~Packet();

  const IPHeader& ip_header() { return ip_header_; }
  const TCPHeader& tcp_header() { return tcp_header_; }
  const char* payload() { return payload_; }
  int payload_size() { return payload_size_; }

  void InjectPayload(const char* payload, int size);

 private:
  IPHeader ip_header_;
  TCPHeader tcp_header_;
  char* payload_ = nullptr;
  int payload_size_ = 0;
};

}  // namespace net_stack

#endif  // NET_STACK_PACKET_