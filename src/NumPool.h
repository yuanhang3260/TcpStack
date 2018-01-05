#ifndef NET_STACK_NUM_POOL_
#define NET_STACK_NUM_POOL_

#include <mutex>
#include <set>
#include <string>

#include "Base/BaseTypes.h"

namespace net_stack {

// This is a utility class which allocates number from a pool. It will be used
// by Host and Process class to allocate file descriptor, port, etc. This class
// is thread-safe.
class NumPool {
 public:
  NumPool(const std::string& name, int32 min, int32 max);
  NumPool(int32 min, int32 max);

  // Allocate number from pool.
  int32 Allocate();
  int32 AllocateRandom();
  // Take a specified number from pool, return true if succeed.
  bool Take(int32);
  // Release number back to pool.
  void Release(int32);

 private:
  std::string name_;

  std::set<int32> pool_;
  std::mutex mutex_;
};

}  // namespace net_stack

#endif  // NET_STACK_NUM_POOL_
