#ifndef NET_STACK_TCP_CONTROLLER_
#define NET_STACK_TCP_CONTROLLER_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "BaseChannel.h"
#include "PacketQueue.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

namespace net_stack {

// TCP uses <source_ip, source_port, dest_ip, dest_port> as unique indentifier.
struct TcpControllerKey {
  std::string source_ip;
  int source_port;
  std::string dest_ip;
  int dest_port;

  bool operator==(const TcpControllerKey &other) const { 
    return (source_ip == other.source_ip && source_port == other.source_port &&
            dest_ip == other.dest_ip && dest_port == other.dest_port);
  }

  bool operator<(const TcpControllerKey &other) const {
    if (source_ip != other.source_ip) {
      return source_ip < other.source_ip;
    }
    if (source_port != other.source_port) {
      return source_port < other.source_port;
    }
    if (dest_ip != other.dest_ip) {
      return dest_ip < other.dest_ip;
    }
    if (dest_port != other.dest_port) {
      return dest_port < other.dest_port;
    }

    return false;
  }
};

class TcpController {
 public:
  TcpController(const TcpControllerKey& key,
                Executors::FixedThreadPool* thread_pool);

 private:
  TcpControllerKey key_;

  // Packet receive buffer of this single TCP connection.
  PacketQueue rcv_buffer_;
  std::mutex rcv_buffer_mutex_;
  std::condition_variable rcv_buffer_cv_;

  Executors::FixedThreadPool* thread_pool_;
};

}  // namespace net_stack


namespace std {
template <>
struct hash<net_stack::TcpControllerKey> {
  size_t operator() (const net_stack::TcpControllerKey& socket_id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(socket_id.source_ip) ^
              (int_hasher(socket_id.source_port) << 1)) >> 1) ^
             (((str_hasher(socket_id.dest_ip) << 2) ^
               (int_hasher(socket_id.dest_port) << 2)) >> 2);
  }
};
}  // namespace

#endif  // NET_STACK_TCP_CONTROLLER_
