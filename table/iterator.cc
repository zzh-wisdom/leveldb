// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/iterator.h"

namespace leveldb {

/**
 * @brief Construct a new Iterator:: Iterator object
 * 
 * 设置cleanup头结点的function和next为nullptr
 * 
 */
Iterator::Iterator() {
  cleanup_head_.function = nullptr;
  cleanup_head_.next = nullptr;
}

/**
 * @brief Destroy the Iterator:: Iterator object
 * 
 * 逐个调用cleanup node的清理函数
 * 
 */
Iterator::~Iterator() {
  if (!cleanup_head_.IsEmpty()) {
    cleanup_head_.Run();
    for (CleanupNode* node = cleanup_head_.next; node != nullptr;) {
      node->Run();
      CleanupNode* next_node = node->next;
      delete node;
      node = next_node;
    }
  }
}

/**
 * @brief 注册cleanup函数（三元组）
 * 
 * 放入的顺序是：
 * - 头结点是第一个
 * - 其余结点是按照头插法（除了头结点）的顺序放入的
 * 
 * @param func 
 * @param arg1 
 * @param arg2 
 */
void Iterator::RegisterCleanup(CleanupFunction func, void* arg1, void* arg2) {
  assert(func != nullptr);
  CleanupNode* node;
  if (cleanup_head_.IsEmpty()) {
    node = &cleanup_head_;
  } else {
    node = new CleanupNode();
    node->next = cleanup_head_.next;
    cleanup_head_.next = node;
  }
  node->function = func;
  node->arg1 = arg1;
  node->arg2 = arg2;
}

namespace {

/**
 * @brief 空迭代器的实现
 * 
 * 无任何数据，状态为non-valid。
 * 调用任何获取数据的操作，如key()、Next()等等，都会触发断言错误
 * 
 */
class EmptyIterator : public Iterator {
 public:
  EmptyIterator(const Status& s) : status_(s) {}
  ~EmptyIterator() override = default;

  bool Valid() const override { return false; }
  void Seek(const Slice& target) override {}
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  Slice key() const override {
    assert(false);
    return Slice();
  }
  Slice value() const override {
    assert(false);
    return Slice();
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // anonymous namespace

/**
 * @brief 创建空迭代器
 * 
 * @return Iterator* 
 */
Iterator* NewEmptyIterator() { return new EmptyIterator(Status::OK()); }

/**
 * @brief 创建带有特定状态status的空迭代器
 * 
 * @param status 
 * @return Iterator* 
 */
Iterator* NewErrorIterator(const Status& status) {
  return new EmptyIterator(status);
}

}  // namespace leveldb
