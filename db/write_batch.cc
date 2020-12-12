// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "leveldb/write_batch.h"

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "util/coding.h"

/**
 * \file write_batch.cc
 * @brief batch write implication
 */

namespace leveldb {

/**
 * WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
 * 小端存储方式，与机器无关的存储
 */
static const size_t kHeader = 12;

WriteBatch::WriteBatch() { Clear(); }

WriteBatch::~WriteBatch() = default;

WriteBatch::Handler::~Handler() = default;

/**
 * @brief clear
 * 
 * 将req_清空
 * rep_.resize(kHeader);
 * 
 */
void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

/**
 * @brief 返回rep_.size()
 * 
 * @return size_t 
 */
size_t WriteBatch::ApproximateSize() const { return rep_.size(); }

/**
 * @brief 将batch中的操作依次执行
 * 
 * 根据类型，对应Put或者Delete \n
 * 遍历WriteBatch解析成valuetype/key/value/的形式，然后调用相应的Handler接口写入 \n
 * Delete操作只写入删除的key，ValueType是KTypeDeletion，表示key以及被删除，后续compaction会删除此key-value。
 * 
 * @param handler 
 * @return Status 
 */
Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value;
  int found = 0;
  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

/**
 * @brief 计算WriteBatch中记录的个数
 * 
 * 将WriteBatch中rep_的第8-11的四个字节解码成一个uint32的整数
 * 
 * @param b 
 * @return int 
 */
int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

/**
 * @brief 设置WriteBatch的记录个数为n
 * 
 * @param b 
 * @param n 
 */
void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

/**
 * @brief 获取WriteBatch的序列号
 * 
 * @param b 
 * @return SequenceNumber 
 */
SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

/**
 * @brief 设置WriteBatch的序列号为seq
 * 
 * @param b 
 * @param seq 
 */
void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

/**
 * @brief Put
 * 
 * 1. 记录个数加一
 * 2. 添加记录的类型kTypeValue
 * 3. 依次添加带有前缀长度的key和value字符串
 * 
 * @param key 
 * @param value 
 */
void WriteBatch::Put(const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

/**
 * @brief 添加一条删除key的记录
 * 
 * @param key 
 */
void WriteBatch::Delete(const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

/**
 * @brief 附加source中的记录
 * 
 * @param source 
 */
void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::Append(this, &source);
}

namespace {
/**
 * @brief MemTable插入器
 * 
 */
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;

  /**
   * @brief 向mem_插入一条记录，sequence_++
   * 
   * @param key 
   * @param value 
   */
  void Put(const Slice& key, const Slice& value) override {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
  }
  /**
   * @brief 从mem_删除一条记录，sequence_++
   * 
   * @param key 
   */
  void Delete(const Slice& key) override {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};
}  // namespace

/**
 * @brief 插入器，将 WriteBatch 中的数据写入到 MemTable
 * 
 * @param b 
 * @param memtable 
 * @return Status 
 */
Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}

/**
 * @brief SetContents
 * 
 * 将 b->rep_ 设置为 contents
 * 
 * @param b 
 * @param contents 
 */
void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

/**
 * @brief 将src中的记录附加到dst中
 * 
 * @param dst 
 * @param src 
 */
void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace leveldb
