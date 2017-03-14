#ifndef NET_STACK_TCP_WINDOW_
#define NET_STACK_TCP_WINDOW_

#include <memory>
#include <string>

#include "Base/BaseTypes.h"
#include "Base/MacroUtils.h"

namespace net_stack {

// Base class of SendWindow and RecvWindow.
class TcpWindow {
 public:
  TcpWindow() = default;

  static uint32 kDefaultWindowSize;
};

}  // namespace net_stack

#endif  // NET_STACK_TCP_WINDOW_
