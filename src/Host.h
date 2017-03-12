#ifndef NET_STACK_HOST_
#define NET_STACK_HOST_

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Base/MacroUtils.h"
#include "BaseChannel.h"
#include "PacketQueue.h"
#include "TcpController.h"
#include "Utility/ThreadPool.h"

namespace net_stack {

class Host {
 public:
  Host(const std::string& ip_address);

  DEFINE_ACCESSOR(ip_address, std::string);

 private:
  std::string ip_address_;  // human readable IP address (aa.bb.cc.dd)

  // Receive packets buffer.
  PacketQueue rcv_buffer_;
  std::mutex rcv_buffer_mutex_;
  std::condition_variable rcv_buffer_cv_;

  // Send packets buffer.
  PacketQueue send_buffer_;
  std::mutex send_buffer_mutex_;
  std::condition_variable send_buffer_cv_;

  // All TCP connections maintained by this host.
  using TcpConnectionMap = 
      std::unordered_map<TcpControllerKey, std::unique_ptr<TcpController>>;
  TcpConnectionMap connections_;

  // Thread pool. We create a top-level thread pool to manage all threads.
  // It should be passed to all TCP connections for usage.
  Executors::FixedThreadPool thread_pool_;
};

}  // namespace net_stack

#endif  // NET_STACK_HOST_
