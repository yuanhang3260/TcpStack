#ifndef NET_STACK_PACKET_
#define NET_STACK_PACKET_

#include <string>

#include "Base/BaseTypes.h"
#include "IPHeader.h"
#include "TcpHeader.h"
#include "Utility/BufferInterface.h"

namespace net_stack {

// This class is a abstraction of IP packet, which contains TCP/IP header and
// application payload. We use this as the low-level network packet.
//
// This class is not thread-safe.
class Packet {
 public:
  Packet(const IPHeader& ip_header, const TcpHeader& tcp_header);
  Packet(const IPHeader& ip_header, const TcpHeader& tcp_header,
         const std::string& payload);
  // It does NOT take ownership of the payload buffer.
  Packet(const IPHeader& ip_header, const TcpHeader& tcp_header,
         const byte* payload, int size);

  ~Packet();

  Packet* Copy() const;

  const IPHeader& ip_header() { return ip_header_; }
  const TcpHeader& tcp_header() { return tcp_header_; }
  const byte* payload() { return payload_; }
  byte* mutable_payload() { return payload_; }
  uint32 payload_size() { return payload_size_; }

  uint32 InjectPayload(const byte* payload, int size);
  uint32 InjectPayloadFromBuffer(BufferInterface* src_buffer, uint32 size);

 private:
  IPHeader ip_header_;
  TcpHeader tcp_header_;
  byte* payload_ = nullptr;
  uint32 payload_size_ = 0;
};

}  // namespace net_stack

#endif  // NET_STACK_PACKET_