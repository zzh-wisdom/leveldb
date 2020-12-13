// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/arena.h"

namespace leveldb {

static const int kBlockSize = 4096;

Arena::Arena()
    : alloc_ptr_(nullptr), alloc_bytes_remaining_(0), memory_usage_(0) {}

/**
 * @brief Destroy the Arena:: Arena object
 * 
 * 统一释放所有block
 * 
 */
Arena::~Arena() {
  for (size_t i = 0; i < blocks_.size(); i++) {
    delete[] blocks_[i];
  }
}

/**
 * @brief AllocateFallback
 * 
 * 重新羽分配一个内存 block
 * 
 * - kBlockSize=4K（1个page），当bytes大于1/4页时，按照实际需要分配内存，且作为独立的一整块
 * - 否则，新分配一个4k的block，在这个block上进行用户的内存分配
 * 
 * @param bytes 
 * @return char* 
 */
char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Object is more than a quarter of our block size.  Allocate it separately
    // to avoid wasting too much space in leftover bytes.
    char* result = AllocateNewBlock(bytes);
    return result;
  }

  // We waste the remaining space in the current block.
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

/**
 * @brief AllocateAligned
 * 
 * 对齐分配
 * 
 * @param bytes 
 * @return char* 
 */
char* Arena::AllocateAligned(size_t bytes) {
  const int align = (sizeof(void*) > 8) ? sizeof(void*) : 8;  // 更加兼容，以防大于64位的机器？？
  static_assert((align & (align - 1)) == 0,
                "Pointer size should be a power of 2");
  size_t current_mod = reinterpret_cast<uintptr_t>(alloc_ptr_) & (align - 1); // 相当于取余数，计算不对齐部分的大小
  size_t slop = (current_mod == 0 ? 0 : align - current_mod);  // 需要补齐的字节数
  size_t needed = bytes + slop;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + slop;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // AllocateFallback always returned aligned memory
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (align - 1)) == 0);
  return result;
}

/**
 * @brief AllocateNewBlock
 * 
 * 分配新的block，加入到blocks_ vector中，更新memory_usage_
 * 
 * new函数总是对齐分配内存？？
 * 
 * @param block_bytes 
 * @return char* 
 */
char* Arena::AllocateNewBlock(size_t block_bytes) {
  char* result = new char[block_bytes];
  blocks_.push_back(result);
  // 这里难道是要把blocks_数组中保存的char*也算进去？
  memory_usage_.fetch_add(block_bytes + sizeof(char*),
                          std::memory_order_relaxed);
  return result;
}

}  // namespace leveldb
