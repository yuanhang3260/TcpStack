#ifndef NET_STACK_IP_HEADER_
#define NET_STACK_IP_HEADER_

#include <string>

#include "Base/BaseTypes.h"

namespace net_stack {

// This is a very simplifid IP header. We only care about source and destination
// IP addresses.
struct IPHeader {
  std::string source_ip = "";
  std::string dest_ip = "";
  uint16 protocol = 6;  // TCP
};

}  // namespace net_stack

#endif  // NET_STACK_IP_HEADER_
