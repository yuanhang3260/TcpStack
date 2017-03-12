#include "TcpController.h"


namespace net_stack {

TcpController::TcpController(const TcpControllerKey& key,
                             Executors::FixedThreadPool* thread_pool) :
    key_(key),
    thread_pool_(thread_pool) {
}

}  // namespace net_stack
