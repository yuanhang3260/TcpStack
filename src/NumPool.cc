#include "NumPool.h"

NumPool::NumPool(int min, int max) {
  for (int32 i = min; i <= max; i++) {
    pool_.insert(i);
  }
}

int32 NumPool::Allocate() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (pool_.empty()) {
    return -1;
  }

  // Get the first (smallest) number available.
  auto it = pool_.begin();
  int32 num = *it;
  pool_.erase(it);
  return num;
}

bool NumPool::Take(int32 num) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = pool_.find(num);
  if (it == pool_.end()) {
    return false;
  }
  pool_.erase(it);
  return true;
}

int32 NumPool::AllocateRandom() {
  std::unique_lock<std::mutex> lock(mutex_);
  uint32 size = pool_.size();
  if (size == 0) {
    return -1;
  }
  
  // Select a random port from port pool.
  uint32 index = Utils::RandomNumber(size);
  auto it = pool_.begin();
  advance(it, index);
  uint32 num = *it;
  pool_.erase(it);
  return num;
}

void Host::Release(int32 num) {
  std::unique_lock<std::mutex> lock(mutex_);
  pool_.insert(num);
}