// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/merger.h"

#include "leveldb/comparator.h"
#include "leveldb/iterator.h"
#include "table/iterator_wrapper.h"

namespace leveldb {

namespace {
/**
 * @brief MergingIterator
 * 
 * 每次Next或者Prev移动时，都要重新遍历所有的子Iterator以找到key最小或最大的Iterator作为current_。这就是merge的语义所在了。
 * 但是它没有考虑到删除标记等问题，因此直接使用MergingIterator是不能正确的遍历DB的，这些问题留待给DBIter来解决。
 * 
 */
class MergingIterator : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, Iterator** children, int n)
      : comparator_(comparator),
        children_(new IteratorWrapper[n]),
        n_(n),
        current_(nullptr),
        direction_(kForward) {
    for (int i = 0; i < n; i++) {
      children_[i].Set(children[i]);
    }
  }

  ~MergingIterator() override { delete[] children_; }

  /**
   * @brief return current_ != nullptr
   * 
   * @return true 
   * @return false 
   */
  bool Valid() const override { return (current_ != nullptr); }

  /**
   * @brief 数组中的每个迭代器都SeekToFirst，然后寻找最小的key
   * 
   * direction_ 改为 kForward
   * 
   */
  void SeekToFirst() override {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToFirst();
    }
    FindSmallest();
    direction_ = kForward;
  }

  /**
   * @brief 数组中的每个迭代器都SeekToLast，然后寻找最大的key
   * 
   * direction_ 改为 kReverse
   * 
   */
  void SeekToLast() override {
    for (int i = 0; i < n_; i++) {
      children_[i].SeekToLast();
    }
    FindLargest();
    direction_ = kReverse;
  }

  /**
   * @brief 数组中的每个迭代器都Seek，然后寻找最小的key
   * 
   * direction_ 改为 kForward
   * 
   * @param target 
   */
  void Seek(const Slice& target) override {
    for (int i = 0; i < n_; i++) {
      children_[i].Seek(target);
    }
    FindSmallest();
    direction_ = kForward;
  }

  /**
   * @brief Next
   * 
   */
  void Next() override {
    assert(Valid());

    // Ensure that all children are positioned after key().
    // If we are moving in the forward direction, it is already
    // true for all of the non-current_ children since current_ is
    // the smallest child and key() == current_->key().  Otherwise,
    // we explicitly position the non-current_ children.
    // 确保所有的子Iterator都定位在key()之后.
    // 如果我们在正向移动，对于除current_外的所有子Iterator这点已然成立，因为current_是最小的子Iterator，并且key() = current_->key()。
    // 否则，我们需要明确设置其它的子Iterator
    if (direction_ != kForward) {  // 除了current，其他迭代器都要seek操作，然后调整
      for (int i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          child->Seek(key());
          // 这是为了确保其他迭代器的key都比current的key要大，按理用该不会出现相等的情况，因为有全局唯一的seq
          if (child->Valid() &&
              comparator_->Compare(key(), child->key()) == 0) { 
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }

    // current也向后移一位，然后再查找key最小的Iterator
    current_->Next();
    FindSmallest();
  }

  /**
   * @brief Prev
   * 
   */
  void Prev() override {
    assert(Valid());

    // Ensure that all children are positioned before key().
    // If we are moving in the reverse direction, it is already
    // true for all of the non-current_ children since current_ is
    // the largest child and key() == current_->key().  Otherwise,
    // we explicitly position the non-current_ children.
    // 确保所有的子Iterator都定位在key()之前.
    // 如果我们在逆向移动，对于除current_外的所有子Iterator这点已然成立，因为current_是最大的，并且key() = current_->key()
    // 否则，我们需要明确设置其它的子Iterator
    if (direction_ != kReverse) {
      for (int i = 0; i < n_; i++) {
        IteratorWrapper* child = &children_[i];
        if (child != current_) {
          child->Seek(key());
          if (child->Valid()) {
            // Child is at first entry >= key().  Step back one to be < key()
            // child位于>=key()的第一个entry上，prev移动一位到<key()
            child->Prev();
          } else {
            // Child has no entries >= key().  Position at last entry.
            // child所有的entry都 < key()，直接seek到last即可
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }

    // current也向前移一位，然后再查找key最大的Iterator
    current_->Prev();
    FindLargest();
  }

  /**
   * @brief 返回current_迭代器的key
   * 
   * @return Slice 
   */
  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  /**
   * @brief 返回current_迭代器的value
   * 
   * @return Slice 
   */
  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  /**
   * @brief 只有数组中的所有迭代器都ok才返回ok
   * 
   * @return Status 
   */
  Status status() const override {
    Status status;
    for (int i = 0; i < n_; i++) {
      status = children_[i].status();
      if (!status.ok()) {
        break;
      }
    }
    return status;
  }

 private:
  // Which direction is the iterator moving?
  enum Direction { kForward, kReverse };

  void FindSmallest();
  void FindLargest();

  // We might want to use a heap in case there are lots of children.
  // For now we use a simple array since we expect a very small number
  // of children in leveldb.
  const Comparator* comparator_;
  IteratorWrapper* children_;  // 目前最多只有 2（两个mem）+4（L0table）+ 6（7-1层）
  int n_;
  IteratorWrapper* current_;  // 保存所在的迭代器
  Direction direction_;
};

/**
 * @brief 从0开始向后遍历内部Iterator数组，找到key最小的Iterator，并设置到current_
 * 
 */
void MergingIterator::FindSmallest() {
  IteratorWrapper* smallest = nullptr;
  for (int i = 0; i < n_; i++) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (smallest == nullptr) {
        smallest = child;
      } else if (comparator_->Compare(child->key(), smallest->key()) < 0) {
        smallest = child;
      }
    }
  }
  current_ = smallest;
}

/**
 * @brief 从最后一个向前遍历内部Iterator数组，找到key最大的Iterator，并设置到current_
 * 
 */
void MergingIterator::FindLargest() {
  IteratorWrapper* largest = nullptr;
  for (int i = n_ - 1; i >= 0; i--) {
    IteratorWrapper* child = &children_[i];
    if (child->Valid()) {
      if (largest == nullptr) {
        largest = child;
      } else if (comparator_->Compare(child->key(), largest->key()) > 0) {
        largest = child;
      }
    }
  }
  current_ = largest;
}
}  // namespace

/**
 * @brief 创建一个合并迭代器
 * 
 * 如果n=0，返回一个空迭代器
 * 如果n=1，直接返回children[0]
 * 
 * 功能：根据调用者指定的key和sequence，从这些Iterator中找到合适的记录
 * 
 * @param comparator internal_comparator
 * @param children 迭代器数组
 * @param n 
 * @return Iterator* 
 */
Iterator* NewMergingIterator(const Comparator* comparator, Iterator** children,
                             int n) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyIterator();
  } else if (n == 1) {
    return children[0];
  } else {
    return new MergingIterator(comparator, children, n);
  }
}

}  // namespace leveldb
