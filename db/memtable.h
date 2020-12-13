// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include <string>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/db.h"
#include "util/arena.h"

namespace leveldb {

class InternalKeyComparator;
class MemTableIterator;

/**
 * @brief MemTable
 * 
 * MemTable具有引用计数，初始值为0，调用者必须至少调用一次 Ref()。
 * 如果引用计数为0，则在Unref中删除自己。
 * 
 */
class MemTable {
 public:
  /// @details
  /// MemTables are reference counted.  The initial reference count
  /// is zero and the caller must call Ref() at least once.
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  /// Increase reference count.
  void Ref() { ++refs_; }

  /// Drop reference count.  Delete if no more references exist.
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  // Returns an estimate of the number of bytes of data in use by this
  // data structure. It is safe to call when MemTable is being modified.
  /**
   * @brief 估计该结构体使用的字节数
   * 
   * @attention 可以线程安全地调用，即使MemTable正在被修改。
   * 
   * @return size_t 
   */
  size_t ApproximateMemoryUsage();

  /// @brief Return an iterator that yields the contents of the memtable.
  ///
  /// The keys returned by this iterator are internal keys encoded 
  /// by AppendInternalKey in the db/format.{h,cc} module.
  /// 
  /// @attention The caller must ensure that the underlying MemTable remains live
  /// while the returned iterator is live.  
  Iterator* NewIterator();

  /// Add an entry into memtable that maps key to value at the
  /// specified sequence number and with the specified type.
  /// Typically value will be empty if type==kTypeDeletion.
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  /// If memtable contains a value for key, store it in *value and return true.
  /// If memtable contains a deletion for key, store a NotFound() error
  /// in *status and return true.
  /// Else, return false.
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  /// @todo friend class 的用法
  friend class MemTableIterator;
  friend class MemTableBackwardIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  typedef SkipList<const char*, KeyComparator> Table;

  ~MemTable();  // Private since only Unref() should be used to delete it

  KeyComparator comparator_;
  int refs_;
  Arena arena_;  /// 初始化时使用无参构造函数构造的
  Table table_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
