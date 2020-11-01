// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/two_level_iterator.h"

#include "leveldb/table.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {

/**
 * @todo 学习如何使用 typedef
 * 
 */
typedef Iterator* (*BlockFunction)(void*, const ReadOptions&, const Slice&);

/**
 * @brief 两层迭代器
 * 
 * 也是Iterator的子类。
 * 之所以叫two level应该是不仅可以迭代其中存储的对象，它还接受了一个函数BlockFunction，
 * 可以遍历存储的对象，可见它是专门为Table定制的。
 * 
 * 即可以同时遍历Index block和对应的每一个data block，内部使用了两个迭代器
 * 
 */
class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function,
                   void* arg, const ReadOptions& options);

  ~TwoLevelIterator() override;

  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;
  void Next() override;
  void Prev() override;

  /**
   * @brief 判断data_iter_是否有效
   * 
   * @return true 
   * @return false 
   */
  bool Valid() const override { return data_iter_.Valid(); }

  /**
   * @brief 返回data_iter_(data block)的key
   * 
   * @return Slice 
   */
  Slice key() const override {
    assert(Valid());
    return data_iter_.key();
  }

  /**
   * @brief 返回data_iter_(data block)的value
   * 
   * @return Slice 
   */
  Slice value() const override {
    assert(Valid());
    return data_iter_.value();
  }

  /**
   * @brief 依次判断index_iter_、data_iter_的status，如果有错误则返回，否则返回类中的status_
   * 
   * @return Status 
   */
  Status status() const override {
    // It'd be nice if status() returned a const Status& instead of a Status
    if (!index_iter_.status().ok()) {
      return index_iter_.status();
    } else if (data_iter_.iter() != nullptr && !data_iter_.status().ok()) {
      return data_iter_.status();
    } else {
      return status_;
    }
  }

 private:
  /**
   * @brief 将状态s(如果出错)保存到status_
   * 
   * @param s 
   */
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }
  void SkipEmptyDataBlocksForward();
  void SkipEmptyDataBlocksBackward();
  void SetDataIterator(Iterator* data_iter);
  void InitDataBlock();

  BlockFunction block_function_; /// block操作函数,读取返回index block中一个entry对应的data block的迭代器
  void* arg_;                    /// BlockFunction的自定义参数，即Table*
  const ReadOptions options_;    /// BlockFunction的read option参数
  Status status_;                /// 当前状态
  IteratorWrapper index_iter_;   /// 遍历index block的迭代器
  IteratorWrapper data_iter_;    /// 遍历data block的迭代器(May be nullptr)
  /// If data_iter_ is non-null, then "data_block_handle_" holds the
  /// "index_value" passed to block_function_ to create the data_iter_.
  std::string data_block_handle_; /// data_iter_ 对应的 data_block_handle
};

TwoLevelIterator::TwoLevelIterator(Iterator* index_iter,
                                   BlockFunction block_function, void* arg,
                                   const ReadOptions& options)
    : block_function_(block_function),
      arg_(arg),
      options_(options),
      index_iter_(index_iter),
      data_iter_(nullptr) {}

TwoLevelIterator::~TwoLevelIterator() = default;

/**
 * @brief 跳转到第一个key大于等于target的位置
 * 
 * @param target 
 */
void TwoLevelIterator::Seek(const Slice& target) {
  index_iter_.Seek(target);
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.Seek(target);
  SkipEmptyDataBlocksForward();
}

/**
 * @brief 跳转到第一个kv
 * 
 */
void TwoLevelIterator::SeekToFirst() {
  index_iter_.SeekToFirst();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  SkipEmptyDataBlocksForward();
}

/**
 * @brief 跳转到最后一个kv
 * 
 */
void TwoLevelIterator::SeekToLast() {
  index_iter_.SeekToLast();
  InitDataBlock();
  if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  SkipEmptyDataBlocksBackward();
}

/**
 * @brief 下一个 entry
 * 
 */
void TwoLevelIterator::Next() {
  assert(Valid());
  data_iter_.Next();
  SkipEmptyDataBlocksForward();
}

/**
 * @brief 上一个 entry
 * 
 */
void TwoLevelIterator::Prev() {
  assert(Valid());
  data_iter_.Prev();
  SkipEmptyDataBlocksBackward();
}

/**
 * @brief 向前跳过空的datablock，直到遇到有效的数据的datablock或者到最末尾
 * 
 * 通常是由于当前block遍历到尾部了，需要跳到下一个block的起始位置
 * 
 */
void TwoLevelIterator::SkipEmptyDataBlocksForward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Next();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToFirst();
  }
}

/**
 * @brief 向后跳过空的datablock
 * 
 * 通常是由于当前block逆序遍历到头部了，需要跳到上一个block的末尾位置
 * 
 */
void TwoLevelIterator::SkipEmptyDataBlocksBackward() {
  while (data_iter_.iter() == nullptr || !data_iter_.Valid()) {
    // Move to next block
    if (!index_iter_.Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    index_iter_.Prev();
    InitDataBlock();
    if (data_iter_.iter() != nullptr) data_iter_.SeekToLast();
  }
}

/**
 * @brief 将data_iter_设置为data_iter
 * 
 * 若data_iter_.iter()不为nullptr，需要SaveError
 * 
 * @param data_iter 
 */
void TwoLevelIterator::SetDataIterator(Iterator* data_iter) {
  if (data_iter_.iter() != nullptr) SaveError(data_iter_.status());
  data_iter_.Set(data_iter);
}

/**
 * @brief 根据index_iter_当前的值，设置data_iter_和data_block_handle_
 * 
 */
void TwoLevelIterator::InitDataBlock() {
  if (!index_iter_.Valid()) {
    SetDataIterator(nullptr);
  } else {
    Slice handle = index_iter_.value();
    if (data_iter_.iter() != nullptr &&
        handle.compare(data_block_handle_) == 0) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } else {
      Iterator* iter = (*block_function_)(arg_, options_, handle);
      data_block_handle_.assign(handle.data(), handle.size());
      SetDataIterator(iter);
    }
  }
}

}  // namespace

/**
 * @brief 建了一个TwoLevelIterator对象
 * 
 * 下面的参数解释以读取data block为例
 * 
 * @param index_iter  index_block的迭代器
 * @param block_function  BlockReader 函数指针
 * @param arg             Table*
 * @param options 
 * @return Iterator* 
 */
Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              BlockFunction block_function, void* arg,
                              const ReadOptions& options) {
  return new TwoLevelIterator(index_iter, block_function, arg, options);
}

}  // namespace leveldb
