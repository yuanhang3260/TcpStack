#include "Common.h"

namespace net_stack {

void Socket::Bind(const LocalLayerThreeKey& key) {
  local_bound = key;
}

void Socket::Bind(const std::string& local_ip, uint32 local_port) {
  local_bound.local_ip = local_ip;
  local_bound.local_port = local_port;
}

bool Socket::isBound() const {
  return !local_bound.local_ip.empty() && local_bound.local_port > 0;
}

void KernelOpenFile::IncRef() {
  refs++;
}

void KernelOpenFile::DecRef() {
  refs--;
}

int32 KernelOpenFile::Refs() {
  return refs.load(std::memory_order_relaxed);
}

}  // namespace net_stack
