// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/dbformat.h"

#include <cstdio>
#include <sstream>

#include "port/port.h"
#include "util/coding.h"

namespace leveldb {

/**
 * @brief 将seq（高7bytes）和t组装成一个uint64
 * 
 * @param seq 
 * @param t
 * @return uint64_t 
 */
static uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);
  assert(t <= kValueTypeForSeek);
  return (seq << 8) | t;
}

/**
 * @brief 将ParsedInternalKey中的key、seq和type附加到result字符串末尾。
 * 
 * @param result 
 * @param key 
 */
void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

/**
 * @brief 将ParsedInternalKey中的内容打印 ’key‘ @seq : type
 * 
 * @return std::string 
 */
std::string ParsedInternalKey::DebugString() const {
  std::ostringstream ss;
  ss << '\'' << EscapeString(user_key.ToString()) << "' @ " << sequence << " : "
     << static_cast<int>(type);
  return ss.str();
}

std::string InternalKey::DebugString() const {
  ParsedInternalKey parsed;
  if (ParseInternalKey(rep_, &parsed)) {
    return parsed.DebugString();
  }
  std::ostringstream ss;
  ss << "(bad)" << EscapeString(rep_);
  return ss.str();
}

const char* InternalKeyComparator::Name() const {
  return "leveldb.InternalKeyComparator";
}

/**
 * @brief InternalKeyComparator
 * 
 * S1 首先比较user key，基于用户设置的comparator，如果user key不相等就直接返回比较结果；否则执行进入S2；
 * S2 取出8字节的sequence number | value type，如果akey的 > bkey的则返回-1，如果akey的<bkey的返回1，相等返回0；
 * 
 * 排序比较规则：
 * - 首先根据user key按升序排列
 * - 然后根据sequence number按降序排列
 * - 最后根据value type按降序排列
 * 
 * 虽然比较时value type并不重要，因为sequence number是唯一的，
 * 但是直接取出8byte的sequence number | value type，然后做比较更方便，不需要再次移位提取出7byte的sequence number。
 * 这也是把value type安排在低7byte的好处吧，排序的两个依据就是user key和sequence number。
 * 
 * @param akey 
 * @param bkey 
 * @return int 
 */
int InternalKeyComparator::Compare(const Slice& akey, const Slice& bkey) const {
  // Order by:
  //    increasing user key (according to user-supplied comparator)
  //    decreasing sequence number
  //    decreasing type (though sequence# should be enough to disambiguate)
  int r = user_comparator_->Compare(ExtractUserKey(akey), ExtractUserKey(bkey));
  if (r == 0) {
    const uint64_t anum = DecodeFixed64(akey.data() + akey.size() - 8);
    const uint64_t bnum = DecodeFixed64(bkey.data() + bkey.size() - 8);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = +1;
    }
  }
  return r;
}

void InternalKeyComparator::FindShortestSeparator(std::string* start,
                                                  const Slice& limit) const {
  // Attempt to shorten the user portion of the key
  Slice user_start = ExtractUserKey(*start);
  Slice user_limit = ExtractUserKey(limit);
  std::string tmp(user_start.data(), user_start.size());
  user_comparator_->FindShortestSeparator(&tmp, user_limit);
  if (tmp.size() < user_start.size() &&
      user_comparator_->Compare(user_start, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    // 使用最大的sequence number，以保证排在相同user key记录序列的第一个
    // 所以seq应该是降序的吧，越大版本越旧？
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(*start, tmp) < 0);
    assert(this->Compare(tmp, limit) < 0);
    start->swap(tmp);
  }
}
/**
 * @brief FindShortSuccessor
 * 
 * 接下来是FindShortSuccessor(std::string* key)函数，该函数取出Internal Key中的user key字段，
 * 根据user指定的comparator找到并替换key，如果key被替换了，就用新的key更新Internal Key，
 * 并使用最大的sequence number。否则保持不变。
 * 
 * @param key 
 */
void InternalKeyComparator::FindShortSuccessor(std::string* key) const {
  Slice user_key = ExtractUserKey(*key);
  std::string tmp(user_key.data(), user_key.size());
  user_comparator_->FindShortSuccessor(&tmp);
  if (tmp.size() < user_key.size() &&
      user_comparator_->Compare(user_key, tmp) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    // 使用最大的sequence number，以保证排在相同user key记录序列的第一个
    PutFixed64(&tmp,
               PackSequenceAndType(kMaxSequenceNumber, kValueTypeForSeek));
    assert(this->Compare(*key, tmp) < 0);
    key->swap(tmp);
  }
}

const char* InternalFilterPolicy::Name() const { return user_policy_->Name(); }

void InternalFilterPolicy::CreateFilter(const Slice* keys, int n,
                                        std::string* dst) const {
  // We rely on the fact that the code in table.cc does not mind us
  // adjusting keys[].
  Slice* mkey = const_cast<Slice*>(keys);
  for (int i = 0; i < n; i++) {
    mkey[i] = ExtractUserKey(keys[i]);
    // TODO(sanjay): Suppress dups?
  }
  user_policy_->CreateFilter(keys, n, dst);
}

bool InternalFilterPolicy::KeyMayMatch(const Slice& key, const Slice& f) const {
  return user_policy_->KeyMayMatch(ExtractUserKey(key), f);
}

/**
 * @details 对于大key会在堆分配新空间，而不使用space_
 */
LookupKey::LookupKey(const Slice& user_key, SequenceNumber s) {
  size_t usize = user_key.size();
  size_t needed = usize + 13;  // A conservative estimate 保守估计，因为usize编码后最长可能达到5bytes
  char* dst;
  if (needed <= sizeof(space_)) {
    dst = space_;
  } else {
    dst = new char[needed];
  }
  start_ = dst;
  dst = EncodeVarint32(dst, usize + 8);
  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  EncodeFixed64(dst, PackSequenceAndType(s, kValueTypeForSeek));
  dst += 8;
  end_ = dst;
}

}  // namespace leveldb
