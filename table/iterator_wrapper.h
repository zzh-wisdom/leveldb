// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_
#define STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_

#include "leveldb/iterator.h"
#include "leveldb/slice.h"

namespace leveldb {

/**
 * @brief Iterator的包装——IteratorWrapper
 * 
 * A internal wrapper class with an interface similar to Iterator that
 * caches the valid() and key() results for an underlying iterator.
 * This can help avoid virtual function calls and also gives better
 * cache locality.
 * 
 * 主要是为了缓存key和valid，避免每次都要调用iterator->key()和iterator->valid()，
 * 因为虚函数调的频繁调用，有一定的性能消耗。至于为何有性能损耗，可参考：
 * - [C++中虚函数(virtual function)到底有多慢](http://www.voidcn.com/article/p-wtkkobku-tc.html)
 * - [为什么 C++ 中使用虚函数时会影响效率？](http://www.voidcn.com/link?url=https://www.zhihu.com/question/22958966)
 * 
 */
class IteratorWrapper {
 public:
  IteratorWrapper() : iter_(nullptr), valid_(false) {}
  explicit IteratorWrapper(Iterator* iter) : iter_(nullptr) { Set(iter); }
  ~IteratorWrapper() { delete iter_; }
  Iterator* iter() const { return iter_; }

  /**
   * @brief 设置iter_
   * 
   * 设置后，调用Update函数，更新valid和key
   * 
   * Takes ownership of "iter" and will delete it when destroyed, or
   * when Set() is invoked again.
   * 
   * @param iter 
   */
  void Set(Iterator* iter) {
    delete iter_;
    iter_ = iter;
    if (iter_ == nullptr) {
      valid_ = false;
    } else {
      Update();
    }
  }

  // Iterator interface methods
  bool Valid() const { return valid_; }
  Slice key() const {
    assert(Valid());
    return key_;
  }
  Slice value() const {
    assert(Valid());
    return iter_->value();
  }
  /// Methods below require iter() != nullptr
  /// 返回iter_->status()
  Status status() const {
    assert(iter_);
    return iter_->status();
  }
  void Next() {
    assert(iter_);
    iter_->Next();
    Update();
  }
  void Prev() {
    assert(iter_);
    iter_->Prev();
    Update();
  }
  void Seek(const Slice& k) {
    assert(iter_);
    iter_->Seek(k);
    Update();
  }
  void SeekToFirst() {
    assert(iter_);
    iter_->SeekToFirst();
    Update();
  }
  void SeekToLast() {
    assert(iter_);
    iter_->SeekToLast();
    Update();
  }

 private:
  /**
   * @brief 根据iter_，更新 valid_ 和 key_
   * 
   */
  void Update() {
    valid_ = iter_->Valid();
    if (valid_) {
      key_ = iter_->key();
    }
  }

  Iterator* iter_;
  bool valid_;
  Slice key_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_ITERATOR_WRAPPER_H_
