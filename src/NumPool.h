#ifndef NET_STACK_NUM_POOL_
#define NET_STACK_NUM_POOL_

#include <set>
#include <mutex>

#include "Base/BaseTypes.h"

namespace net_stack {

// This is a utility class which allocates number from a pool. It will be used
// by Host and Process class to allocate file descriptor, port, etc. This class
// is thread-safe.
class NumPool {
 public:
  NumPool(int32 min, int32 max);

  // Allocate number from pool.
  int32 Allocate();
  int32 AllocateRandom();
  // Take a specified number from pool, return true if succeed.
  bool Take(int32);
  // Release number back to pool.
  void Release(int32);

 private:
  std::set<int32> pool_;
  std::mutex mutex_;
};

}  // namespace net_stack

#endif  // NET_STACK_NUM_POOL_
