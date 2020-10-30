// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

/**
 * \file block_builder.h
 * 
 * @brief Block 构建
 * 
 * \sa 
 *   - 自我整理的文档： @ref  mydocs/table.md
 * 
 */

namespace leveldb {

struct Options;

class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  /// No copying allowed
  BlockBuilder(const BlockBuilder&) = delete;
  /// No copying allowed
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  /// Reset the contents as if the BlockBuilder was just constructed.
  /// 重置
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  /**
   * @pre Finish() has not been called since the last call to Reset().
   * @pre key is larger than any previously added key
   */
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  /**
   * @details Finish构建block并返回引用block内容的切片。
   * 在此构建器的生命周期内或直到调用Reset（）之前，返回的片将保持有效。
   */
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  /// @details 估算当前block data的大小(未压缩)
  size_t CurrentSizeEstimate() const;

  /// @details Return true iff no entries have been added since the last Reset()
  bool empty() const { return buffer_.empty(); }

 private:
  const Options* options_;
  std::string buffer_;              /// Destination buffer 用于存放block data
  std::vector<uint32_t> restarts_;  /// Restart points 用于存放重启点的位置信息
  int counter_;                     /// Number of entries emitted since restart 从上个重启点遍历到(还未到)下个重启点时的计数
  bool finished_;                   /// Has Finish() been called? 是否调用了Finish
  std::string last_key_;            /// 记录最后Add的key
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
