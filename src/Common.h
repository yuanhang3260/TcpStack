#ifndef NET_STACK_COMMON_
#define NET_STACK_COMMON_

#include "Base/BaseTypes.h"

namespace net_stack {

enum SocketState {
  OPEN,
  EOF_NOT_READ,
  EOF_READ,
  CLOSED,
  SHUTDOWN,
};

}  // namespace net_stack

#endif  // NET_STACK_COMMON_
