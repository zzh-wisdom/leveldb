// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"

#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace leveldb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      std::fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      std::fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.
/**
 * @brief DBIter
 * 
 * Leveldb数据库的MemTable和sstable文件的存储格式都是(user key, seq, type) => uservalue。
 * DBIter把同一个userkey在DB中的多条记录合并为一条，综合考虑了userkey的序号、删除标记、和写覆盖等等因素。
 * 
 */
class DBIter : public Iterator {
 public:
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction { kForward, kReverse };

  DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
         uint32_t seed)
      : db_(db),
        user_comparator_(cmp),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        rnd_(seed),
        bytes_until_read_sampling_(RandomCompactionPeriod()) {}

  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  ~DBIter() override { delete iter_; }
  bool Valid() const override { return valid_; }
  /**
   * @brief key
   * 
   * 返回user key
   * kForward： 从iter中得到
   * kReverse： saved_key_
   * 
   * @return Slice 
   */
  Slice key() const override {
    assert(valid_);
    return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
  }
  Slice value() const override {
    assert(valid_);
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }
  Status status() const override {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  void Next() override;
  void Prev() override;
  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);

  /**
   * @brief 将k assign到dst
   * 
   * @param k 
   * @param dst 
   */
  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }

  /**
   * @brief 清空saved_value_
   * 
   */
  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  /// Picks the number of bytes that can be read until a compaction is scheduled.
  /// 选择在计划压缩之前可以读取的字节数。
  /// 调用Uniform函数生成[0...2 * config::kReadBytesPeriod]范围的随机数
  size_t RandomCompactionPeriod() {
    return rnd_.Uniform(2 * config::kReadBytesPeriod);
  }

  DBImpl* db_;
  const Comparator* const user_comparator_;
  Iterator* const iter_;  /// merge iter
  SequenceNumber const sequence_;
  Status status_;
  std::string saved_key_;    /// == current key when direction_==kReverse，否则为上一个被跳过的key（deletion或者正常）
  std::string saved_value_;  /// == current raw value when direction_==kReverse
  Direction direction_;  //   变成kForward，需要清空saved_key_ 和 saved_value_
  bool valid_; 
  Random rnd_;  /// 使用传入的种子seed进行构造
  size_t bytes_until_read_sampling_;  /// +rnd_.Uniform(2 * config::kReadBytesPeriod);
};

/**
 * @brief 根据bytes_until_read_sampling_决定是否进行抽样读，然后将当前iter的key解码到ikey
 * 
 * @param ikey 
 * @return true 
 * @return false 
 */
inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();

  size_t bytes_read = k.size() + iter_->value().size();
  while (bytes_until_read_sampling_ < bytes_read) {  ///@todo 这里读这么多次不会降低scan的效率吗？虽然有缓存，内存读比较快，而且只对大的key value进行抽样读
    bytes_until_read_sampling_ += RandomCompactionPeriod();
    db_->RecordReadSample(k);
  }
  assert(bytes_until_read_sampling_ >= bytes_read);
  bytes_until_read_sampling_ -= bytes_read;

  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  } else {
    return true;
  }
}

/**
 * @brief Next
 * 
 */
void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    // iter_指向this-> key（）的条目之前，因此请进入this-> key（）的条目范围，然后使用下面的常规跳过代码。
    if (!iter_->Valid()) {  // 此时意味着当前迭代器指到比第一个key还小的地方，prev函数造成的
      iter_->SeekToFirst();  // 让iter指向current key
    } else {
      iter_->Next(); // 让iter指向current key
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already contains the key to skip past. // 对于 kReverse，saved_key_保存的就是当前的key
  } else {
    // 而对于kForward，需要手动将当前的key保存到saved_key_
    // Store in saved_key_ the current key so we skip it below.
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

    // iter_ is pointing to current key. We can now safely move to the next to
    // avoid checking current key.
    iter_->Next();  // 让iter指向current key
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  }

  // 此时saved_key_保存当前key，以便被跳过
  FindNextUserEntry(true, &saved_key_);
}

/**
 * @brief 循环跳过下一个delete的记录，也跳过和当前key的user_key相同的key，直到遇到下一个kValueType的记录。
 * 
 * @param skipping 
 * @param skip 保存跳过的deletion key
 */
void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        case kTypeValue:
          if (skipping &&
              user_comparator_->Compare(ikey.user_key, *skip) <= 0) { // 按道理小于的情况不会出现的，等于则说明被覆盖，因为seq是按照降序排的
            // Entry hidden
          } else {  // 找到大于的key且不是deletion，则成功
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

/**
 * @brief Prev
 * 
 * 往回遍历的时候，iter_指向current key的前一个
 * 
 */
void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    while (true) {  // 这里的循环，是为了满足kReverse方向时，iter是指向current key的前一个位置的（userkey更小）
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) <
          0) {  // 若和当前key的user_key部分相等，还需要继续跳过，需要找到下一个不同的user_key
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}
/**
 * @brief 循环跳过下一个delete的记录，也跳过和当前key的user_key相同的key，直到遇到kValueType的记录。
 * 
 */
void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);

  ValueType value_type = kTypeDeletion;  // 保存上一个记录的状态，初始化为kTypeDeletion，就是为了跳过当前key
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {  // 第一次循环不会落入这里
          // We encountered a non-deleted value in entries for previous keys,
          break; // 我们遇到了前一个key的一个未被删除的entry，说明当前保存的key必定没有被覆盖，是有效的，跳出循环
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();  // 此时的key标记为kTypeDeletion，value为null
          ClearSavedValue();
        } else {   // 对于同一个key存在多版本的情况，都会运行到这里，然后同一个user key的value会保存多遍，最后一次保存的是最新的
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) { // 1MB
            std::string empty;
            swap(empty, saved_value_);
          }
          SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // End
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;  // 这里有可能导致 iter_->Valid()为false，而valid_为true，此时迭代器指到比第一个key还小的地方
  }
}

/**
 * @brief seek
 * 
 * @param target 
 */
void DBIter::Seek(const Slice& target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  AppendInternalKey(&saved_key_,
                    ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

}  // anonymous namespace

/**
 * @brief NewDBIterator
 * 
 * 直接调用new DBIter函数
 * 
 * @param db 
 * @param user_key_comparator 
 * @param internal_iter 
 * @param sequence 
 * @param seed 
 * @return Iterator* 
 */
Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator,
                        Iterator* internal_iter, SequenceNumber sequence,
                        uint32_t seed) {
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace leveldb
