// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// A database can be configured with a custom FilterPolicy object.
// This object is responsible for creating a small filter from a set
// of keys.  These filters are stored in leveldb and are consulted
// automatically by leveldb to decide whether or not to read some
// information from disk. In many cases, a filter can cut down the
// number of disk seeks form a handful to a single disk seek per
// DB::Get() call.
//
// Most people will want to use the builtin bloom filter support (see
// NewBloomFilterPolicy() below).

#ifndef STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
#define STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_

#include <string>

#include "leveldb/export.h"

namespace leveldb {

class Slice;

/**
 * @brief FilterPolicy接口
 * 
 * @todo 为什么这里可以使用 Slice，没有看到包含相应的头文件
 * 
 */
class LEVELDB_EXPORT FilterPolicy {
 public:
  virtual ~FilterPolicy();

  /// Return the name of this policy.  
  ///
  /// @note
  /// Note that if the filter encoding
  /// changes in an incompatible way, the name returned by this method
  /// must be changed.  Otherwise, old incompatible filters may be
  /// passed to methods of this type.
  virtual const char* Name() const = 0;

  /// keys[0,n-1] contains a list of keys (potentially with duplicates)
  /// that are ordered according to the user supplied comparator.
  /// Append a filter that summarizes keys[0,n-1] to *dst.
  ///
  /// @warning 
  /// Warning: do not change the initial contents of *dst.  Instead,
  /// append the newly constructed filter to *dst.
  /// 即构建的filter数据不会覆盖dst原有的数据，只会附加到最后
  virtual void CreateFilter(const Slice* keys, int n,
                            std::string* dst) const = 0;

  /// "filter" contains the data appended by a preceding call to
  /// CreateFilter() on this class.  This method must return true if
  /// the key was in the list of keys passed to CreateFilter().
  /// This method may return true or false if the key was not on the
  /// list, but it should aim to return false with a high probability.
  ///
  /// 它不需要精确，也就是即使key不在前面传递的key列表中，也可以返回true，
  /// 但是如果key在列表中，就必须返回true。
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

/// 该函数返回一个使用bloom过滤器的FilterPolicy接口实例
/// Return a new filter policy that uses a bloom filter with approximately
/// the specified number of bits per key.  A good value for bits_per_key
/// is 10, which yields a filter with ~ 1% false positive rate.
///
/// Callers must delete the result after any database that is using the
/// result has been closed.
///
/// @note
/// Note: if you are using a custom comparator that ignores some parts
/// of the keys being compared, you must not use NewBloomFilterPolicy()
/// and must provide your own FilterPolicy that also ignores the
/// corresponding parts of the keys.  For example, if the comparator
/// ignores trailing spaces, it would be incorrect to use a
/// FilterPolicy (like NewBloomFilterPolicy) that does not ignore
/// trailing spaces in keys.
LEVELDB_EXPORT const FilterPolicy* NewBloomFilterPolicy(int bits_per_key);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_FILTER_POLICY_H_
