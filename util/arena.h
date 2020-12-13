// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace leveldb {

/**
 * @brief Arena
 * 
 * 内存池。
 * 内存管理类，只负责内存的分配，而回收只能等到这个类析构的时候。
 * 
 * ## 问题背景：
 * - 对于小对象来说，直接使用new/delete代价比较大，要付出额外的空间和时间，性价比不高。
 * - 另外，我们也要避免多次的申请和释放引起的内存碎片。一旦碎片到达一定程度，
 *   即使剩余内存总量够用，但由于缺乏足够的连续空闲空间，导致内存不够用的假象。
 * 
 * c++ STL为了避免内存碎片，实现一个复杂的内存池，leveldb中则没有那么复杂，只是实现了一个"一次性"内存池Arena。
 * 
 * 在leveldb里面，并不是所有的地方都使用了这个内存池，主要是memtable使用，主要是用于临时存放用户的更新数据，
 * 由于更新的数据可能很小，所以这里使用内存池就很合适。
 * 
 * ## 作用
 * 为了避免小对象的频繁分配，需要减少对new的调用，最简单的做法就是申请大块的内存，多次分给客户。
 * 
 * ## 内存分配策略：
 * leveldb用一个vector<char *>来保存所有的内存分配记录，默认每次申请4k的内存，记录下剩余指针和剩余内存字节数，
 * 每当有新的申请，如果当前剩余的字节能满足需要，则直接返回给用户，如果不能，对于超过1k的请求，直接new返回，
 * 小于1K的请求，则申请一个新的4k块，从中分配一部分给用户。
 * 
 * 但是这样的一个问题就是当前块剩余的部分就浪费了，改进的方法，针对每个block都记录剩余字节，
 * 这样就需要遍历来查找合适的block，要付出一些性能的代价。google的做法是空间浪费就浪费吧：-）以追求更快的性能
 * 
 * 至于释放，需要释放整个内存池来释放所占内存，这个和leveldb的需求有关,memtable不需要释放单次内存，
 * flush到硬盘后整个memtable销毁。
 * 
 */
class Arena {
 public:
  Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  ~Arena();

  /// Return a pointer to a newly allocated memory block of "bytes" bytes.
  char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc.
  char* AllocateAligned(size_t bytes);

  /// Returns an estimate of the total memory usage of data allocated
  /// by the arena. 注意不是用户实际使用的内存大小，还要考虑其他开销
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  char* alloc_ptr_;   /// 上次内存分配的结束位置
  size_t alloc_bytes_remaining_;  /// 当前block中的剩余可用字节数

  // Array of new[] allocated memory blocks
  std::vector<char*> blocks_; ///每个通过new []分配的block的起始地址，不同block大小可以不一样

  // Total memory usage of the arena.
  //
  // TODO(costan): This member is accessed via atomics, but the others are
  //               accessed without any locking. Is this OK?
  std::atomic<size_t> memory_usage_; /// 所有分配的block的总大小，而不是用户实际使用的内存大小
};

/// 直接分配，不管是否对齐
/// @attention 不允许分配0字节大小的内存空间
inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) { //需要内存小于当前block的剩余大小，则直接在该block中分配
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes); //否则申请一个新的block再进行分配
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
