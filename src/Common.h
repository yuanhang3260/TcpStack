#ifndef NET_STACK_COMMON_
#define NET_STACK_COMMON_

#include <atomic>
#include <mutex>
#include <set>

#include "Base/BaseTypes.h"

namespace net_stack {

class TcpController;

struct Socket {
  enum State {
    OPEN,
    EOF_NOT_READ,
    EOF_READ,
    CLOSED,
    SHUTDOWN,
  };

  void Bind(const LocalLayerThreeKey& key);
  void Bind(const std::string& local_ip, uint32 local_port);
  bool isBound() const;

  Socket() = default;

  Socket::State state = OPEN;
  LocalLayerThreeKey local_bound;

  TcpController* tcp_con = nullptr;

  std::mutex mutex_;
};

// This is the entry to simulate the kernel global open file table.
struct KernelOpenFile {
  enum Type {
    REGULAR,
    SOCKET,
    UNKNOWN_FILE_TYPE,
  }

  KernelOpenFile(KernelOpenFile::Type type_arg) : type(type_arg), refs(0) {}

  void IncRef();
  void DecRef();
  int32 Refs();

  KernelOpenFile::Type type = UNKNOWN_FILE_TYPE;
  Socket socket;

  std::atomic_int refs;
};

}  // namespace net_stack

#endif  // NET_STACK_COMMON_
