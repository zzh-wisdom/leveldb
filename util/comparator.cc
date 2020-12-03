// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/comparator.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>

#include "leveldb/slice.h"
#include "util/logging.h"
#include "util/no_destructor.h"

namespace leveldb {

Comparator::~Comparator() = default;

namespace {
/**
 * @brief 字典顺序比较类
 * 
 */
class BytewiseComparatorImpl : public Comparator {
 public:
  /// \todo BytewiseComparatorImpl() = default 中 default 的作用
  BytewiseComparatorImpl() = default;

  /// \todo const char* Name() const override 中 const 的作用
  const char* Name() const override { return "leveldb.BytewiseComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }

  /**
   * @brief 寻找位于[start, limit)最短的分界key
   * 
   * @param start 
   * @param limit 
   */
  void FindShortestSeparator(std::string* start,
                             const Slice& limit) const override {
    // Find length of common prefix
    size_t min_length = std::min(start->size(), limit.size());
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other，不修改，直接返回
    } else {
      // 尝试执行字符start[diff_index]++，设置start长度为diff_index+1，并返回
      // ++条件：字符 < oxff 并且 字符+1 < limit上该index的字符
      // 否则，不做任何处理
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        (*start)[diff_index]++;
        start->resize(diff_index + 1);
        assert(Compare(*start, limit) < 0);
      }
    }
  }
  
  /**
   * @brief 寻找最短的大于等于key的字符串
   * 
   * 找到第一个可以++的字符，执行++后，截断字符串；
   * 如果找不到说明*key的字符都是0xff啊，那就不作修改，直接返回
   * 
   * @param key 
   */
  void FindShortSuccessor(std::string* key) const override {
    // Find first character that can be incremented
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i + 1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
  }
};
}  // namespace

const Comparator* BytewiseComparator() {
  static NoDestructor<BytewiseComparatorImpl> singleton;
  return singleton.get();
}

}  // namespace leveldb
