// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A filter block is stored near the end of a Table file.  It contains
// filters (e.g., bloom filters) for all data blocks in the table combined
// into a single filter block.

#ifndef STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
#define STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

class FilterPolicy;

// A FilterBlockBuilder is used to construct all of the filters for a
// particular Table.  It generates a single string which is stored as
// a special block in the Table.
//
// The sequence of calls to FilterBlockBuilder must match the regexp:
//      (StartBlock AddKey*)* Finish
/**
 * @brief filter block builder
 * 
 * - 实质上就是SST文件里面的meta block，由于目前只有这么一种meta block并用是用来做为filter。所以也经常被称之为filter block。
 * - FilterBlockBuilder用于构造表的所有过滤器。它生成一个字符串，该字符串作为特殊块存储在表中。
 * - 对FilterBlockBuilder的调用顺序必须与regexp相匹配：
 * ```
 * (StartBlock AddKey*)* Finish
 * ```
 * 即，一次BuildBlock中，可以调用多次StartBlock，而调用一次StartBlock可以调用多次AddKey
 * 
 */
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy*);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;   /// 过滤的策略
  std::string keys_;             /// Flattened key contents 所有展开的key(没有压缩)，一个紧挨着一个，\b 每生成一个filter会清空
  std::vector<size_t> start_;    /// Starting index in keys_ of each key 每个 key 在 key_ 的偏移，\b 每生成一个filter会清空
  std::string result_;           /// Filter data computed so far 所有 key 生成的 filter 数据，包含多个 filter
  std::vector<Slice> tmp_keys_;  /// policy_->CreateFilter() argument， 用于暂时存储当前生成filter的所有key
  std::vector<uint32_t> filter_offsets_; /// 存储每个 filter 在 result_ 的偏移
};

class FilterBlockReader {
 public:
  // REQUIRES: "contents" and *policy must stay live while *this is live.
  /**
   * @brief Construct a new Filter Block Reader object
   * 
   * \pre "contents" and *policy must stay live while *this is live.
   * 
   * @param policy 
   * @param contents 
   */
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);
  bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  const FilterPolicy* policy_;
  const char* data_;    /// Pointer to filter data (at block-start)
  const char* offset_;  /// Pointer to beginning of offset array (at block-end)
  size_t num_;          /// Number of entries in offset array
  size_t base_lg_;      /// Encoding parameter (see kFilterBaseLg in .cc file)
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FILTER_BLOCK_H_
