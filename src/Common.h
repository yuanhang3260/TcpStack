#ifndef NET_STACK_COMMON_
#define NET_STACK_COMMON_

#include <set>
#include <string>

#include "Base/BaseTypes.h"
#include "Strings/Utils.h"

namespace net_stack {

// TCP uses <source_ip, source_port, dest_ip, dest_port> as unique indentifier.
struct TcpControllerKey {
  TcpControllerKey() = default;
  TcpControllerKey(const std::string& source_ip_arg,
                   int32 source_port_arg,
                   const std::string& dest_ip_arg,
                   int32 dest_port_arg) :
      source_ip(source_ip_arg),
      source_port(source_port_arg),
      dest_ip(dest_ip_arg),
      dest_port(dest_port_arg) {}

  std::string source_ip;
  int32 source_port = 0;
  std::string dest_ip;
  int32 dest_port = 0;

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

  std::string DebugString() const {
    return Strings::StrCat("{", source_ip, ":", std::to_string(source_port),
                           ", ", dest_ip, ":", std::to_string(dest_port), "}");
  }
};

// Local listener key. It only bundles {local ip, local_port} as key.
struct LocalLayerThreeKey {
  LocalLayerThreeKey() = default;
  LocalLayerThreeKey(const std::string& local_ip_arg,
                     int32 local_port_arg) :
      local_ip(local_ip_arg),
      local_port(local_port_arg) {}

  std::string local_ip;
  int32 local_port = 0;

  bool operator==(const LocalLayerThreeKey &other) const { 
    return local_ip == other.local_ip && local_port == other.local_port;
  }

  bool operator<(const LocalLayerThreeKey &other) const {
    if (local_ip != other.local_ip) {
      return local_ip < other.local_ip;
    }
    if (local_port != other.local_port) {
      return local_port < other.local_port;
    }

    return false;
  }

  std::string DebugString() const {
    return Strings::StrCat("{", local_ip, ", ",
                           std::to_string(local_port), "}");
  }
};

}  // namespace net_stack

namespace std {
template <>
struct hash<net_stack::TcpControllerKey> {
  size_t operator() (const net_stack::TcpControllerKey& tcp_id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(tcp_id.source_ip) ^
              (int_hasher(tcp_id.source_port) << 1)) >> 1) ^
             (((str_hasher(tcp_id.dest_ip) << 2) ^
               (int_hasher(tcp_id.dest_port) << 2)) >> 2);
  }
};

template <>
struct hash<net_stack::LocalLayerThreeKey> {
  size_t operator() (const net_stack::LocalLayerThreeKey& id) const {
    std::hash<std::string> str_hasher;
    std::hash<int> int_hasher;
      return ((str_hasher(id.local_ip) ^
              (int_hasher(id.local_port) << 1)) >> 1);
  }
};

}  // namespace std

#endif  // NET_STACK_COMMON_
