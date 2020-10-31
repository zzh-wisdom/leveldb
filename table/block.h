// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_H_

#include <cstddef>
#include <cstdint>

#include "leveldb/iterator.h"

namespace leveldb {

struct BlockContents;
class Comparator;

/**
 * @brief Block 类
 * 
 * block数据的解码读取是通过该类实现的
 * 
 * \sa 
 *   - 自我整理的文档： @ref  mydocs/table.md
 * 
 */
class Block {
 public:
  /// Initialize the block with the specified contents.
  explicit Block(const BlockContents& contents);

  /// Block 类不可拷贝
  Block(const Block&) = delete;
  /// Block 类不可拷贝
  Block& operator=(const Block&) = delete;

  ~Block();

  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  class Iter;  // 隐藏 block 内部 迭代器实现的细节

  uint32_t NumRestarts() const;

  const char* data_;         /// block数据指针
  size_t size_;              /// block数据大小
  uint32_t restart_offset_;  /// Offset in data_ of restart array 重启点数组在data_中的偏移
  bool owned_;               /// Block owns data_[]  data_[]是否是Block拥有的
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_H_
