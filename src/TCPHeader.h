#ifndef NET_STACK_TCP_HEADER_
#define NET_STACK_TCP_HEADER_

#include "Base/BaseTypes.h"

namespace net_stack {

struct TCPHeader {
  uint16 source_port = 0;
  uint16 dest_port = 0;
  uint32 seq_num = 0;
  uint32 ack_num = 0;
  uint16 data_offset = 0;
  bool ack = false;
  uint16 window_size = 0;
  uint16 check_sum = 0;
};

}  // namespace net_stack

#endif  // NET_STACK_IP_HEADER_