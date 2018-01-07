#ifndef NET_STACK_DEBUG_
#define NET_STACK_DEBUG_

#include <string>

#include "Base/Log.h"

#define LOG(msg, ...)  \
    LogINFO((hostname() + ": " + msg).c_str(), ## __VA_ARGS__);  \

#endif  // NET_STACK_DEBUG_
