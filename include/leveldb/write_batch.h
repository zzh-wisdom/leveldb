// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include <string>

#include "leveldb/export.h"
#include "leveldb/status.h"
/**
 * \file write_batch.h
 */

namespace leveldb {

class Slice;

/**
 * @brief WriteBatch
 * 
 * leveldb使用WriteBatch来替代简单的异步写操作，首先将所有的写操作记录到一个batch中，然后执行同步写，这样同步写的开销就被分散到多个写操作中。
 * 
 * 利用WriteBatch可以进行批量处理
 * WriteBatch的原理是先将所有的操作记录下来，然后再一起操作。
 * 
 * \sa 
 *   - 网上博客： [leveldb之WriteBatch](https://blog.csdn.net/u012658346/article/details/45341885)
 * 
 * batch中record的格式：
 * WriteBatch::rep_ :=
 *    sequence: fixed64
 *    count: fixed32
 *    data: record[count]
 * record :=
 *    kTypeValue varstring varstring         |
 *    kTypeDeletion varstring
 * varstring :=
 *    len: varint32
 *    data: uint8[len]
 * 
 * 每一个WriteBatch都是以一个固定长度的头部开始，然后后面接着许多连续的记录，
 * - 固定头部共12字节，其中前8字节为WriteBatch的序列号，即第一条记录的序列号，对应rep_[0-7],每次处理Batch中的记录时（即BuildBatchGroup时）才会设置（更新）.
 * - 后四字节为当前Batch中的记录数count，对应rep_[8-11]
 * 
 * 示意图如下：
 * 
 * <div align=center><img src="../images/writebatch的记录格式.jpg" height="50%" width="50%"/></div>
 * 
 */
class LEVELDB_EXPORT WriteBatch {
 public:
  /**
   * @brief 处理插入删除记录的接口类
   * 
   */
  class LEVELDB_EXPORT Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  /// Intentionally copyable.  可以被复制
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  /// Store the mapping "key->value" in the database.
  void Put(const Slice& key, const Slice& value);

  /// If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(const Slice& key);

  /// Clear all updates buffered in this batch.
  void Clear();

  /// The size of the database changes caused by this batch.
  ///此次batch造成database变化的大小
  ///
  /// This number is tied to implementation details, and may change across
  /// releases. It is intended for LevelDB usage metrics.
  size_t ApproximateSize() const;

  // Copies the operations in "source" to this batch.
  //
  // This runs in O(source size) time. However, the constant factor is better
  // than calling Iterate() over the source batch with a Handler that replicates
  // the operations into this batch.
  void Append(const WriteBatch& source);

  // Support for iterating over the contents of a batch.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;  // See comment in write_batch.cc for the format of rep_
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
