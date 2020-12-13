// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SKIPLIST_H_
#define STORAGE_LEVELDB_DB_SKIPLIST_H_

/**
 * @file skiplist.h
 * @brief skiplist class definition.
 * 
 * Thread safety
 * ----
 * - 写操作：需要外部同步，比如使用mutex。 
 * - 读操作：需要保证在读取过程中不会破坏SkipList。 除此之外，读取进度无需任何内部锁定或同步。
 * 
 * skiplist的多线程写不安全，但多线程读是安全的。通俗来讲就是，Skiplist同时只允许一个写线程，但允许多个读线程。
 * 内部不使用任何锁来同步，提高了效率。
 * 
 * \invariant 
 * 1. 在销毁SkipList之前，绝不会删除已分配的节点。 由于我们从不删除任何skiplist列表节点，代码可以保证这一点。
 *    即这里通过插入一条删除标志记录来间接实现删除
 * 2. 将节点链接到SkipList之后，除next/prev指针之外的Node的内容是不变的。 
 *    只有Insert（）会修改列表，该方法谨慎地初始化节点并使用release-stores将节点发布到一个或多个列表中。
 * 3. skiplist是有序的
 */

// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the SkipList will not be destroyed
// while the read is in progress.  Apart from that, reads progress
// without any internal locking or synchronization.
//
// Invariants:
//
// (1) Allocated nodes are never deleted until the SkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the SkipList.
// Only Insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
//
// ... prev vs. next pointer ordering ...

#include <atomic>
#include <cassert>
#include <cstdlib>

#include "util/arena.h"
#include "util/random.h"

namespace leveldb {

class Arena;

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  /// Create a new SkipList object that will use "cmp" for comparing keys,
  /// and will allocate memory using "*arena".  Objects allocated in the arena
  /// must remain allocated for the lifetime of the skiplist object.
  explicit SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  /// Insert key into the list.
  /// REQUIRES: nothing that compares equal to key is currently in the list.
  void Insert(const Key& key);

  // Returns true iff an entry that compares equal to key is in the list.
  bool Contains(const Key& key) const;

  /// Iteration over the contents of a skip list
  /// 其实就是简单遍历一个单向链表
  /// 由于没有同步机制，先前创建的迭代器，有可能会遍历到后续插入的key
  class Iterator {
   public:
    /// Initialize an iterator over the specified list.
    /// The returned iterator is not valid.
    explicit Iterator(const SkipList* list);

    /// Returns true iff the iterator is positioned at a valid node.
    bool Valid() const;

    /// Returns the key at the current position.
    /// REQUIRES: Valid()
    const Key& key() const;

    /// Advances to the next position.
    /// REQUIRES: Valid()
    void Next();

    /// Advances to the previous position.
    /// REQUIRES: Valid()
    void Prev();

    /// Advance to the first entry with a key >= target
    void Seek(const Key& target);

    /// Position at the first entry in list.
    /// Final state of iterator is Valid() iff list is not empty.
    void SeekToFirst();

    /// Position at the last entry in list.
    /// Final state of iterator is Valid() iff list is not empty.
    void SeekToLast();

   private:
    const SkipList* list_;
    Node* node_;  /// 当前遍历到的节点
    // Intentionally copyable
  };

 private:
  /// 使用枚举类型定义skiplist的最高高度
  enum { kMaxHeight = 12 };

  /**
   * @brief 获取跳表的最大高度
   * 
   * @return int 
   */
  inline int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

  /// Return true if key is greater than the data stored in "n"
  bool KeyIsAfterNode(const Key& key, Node* n) const;

  /// Return the earliest node that comes at or after key.
  /// Return nullptr if there is no such node.
  ///
  /// If prev is non-null, fills prev[level] with pointer to previous
  /// node at "level" for every level in [0..max_height_-1].
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  /// Return the latest node with a key < key.
  /// Return head_ if there is no such node.
  Node* FindLessThan(const Key& key) const;

  /// Return the last node in the list.
  /// Return head_ if list is empty.
  Node* FindLast() const;

  // Immutable after construction
  Comparator const compare_;  /// 用户定制的比较器
  Arena* const arena_;  /// Arena used for allocations of nodes，内存分配器

  Node* const head_;  /// 前置哨兵节点

  // Modified only by Insert().  Read racily by readers, but stale
  // values are ok.
  /// 仅由Insert（）修改。 读者可以狂躁地阅读，但过时的值还可以。
  std::atomic<int> max_height_;  /// Height of the entire list，记录当前skiplist使用的最高高度

  /// Read/written only by Insert(). 用于产生随机数
  Random rnd_;
};

// Implementation details follow
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}
  /// Node 存储的内容，顶层的key和value都通过编码放置到该成员中
  Key const key;

  /// Accessors/mutators for links.  Wrapped in methods so we can
  /// add the appropriate barriers as necessary.
  /// 获取当前节点在指定level的下一个节点
  Node* Next(int n) {
    assert(n >= 0);
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    return next_[n].load(std::memory_order_acquire);
  }
  /// 将当前节点在指定level的下一个节点设置为x
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    next_[n].store(x, std::memory_order_release);
  }

  // No-barrier variants that can be safely used in a few locations.
  /// 无内存屏障版本，获取当前节点在指定level的下一个节点
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  /// 无内存屏障版本，将当前节点在指定level的下一个节点设置为x
  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  /// Array of length equal to the node height.  next_[0] is lowest level link.
  /// 当前节点的下一个节点数组当前节点的下一个节点数组
  std::atomic<Node*> next_[1];
};

/**
 * @brief 申请一个Node，使用height预分配next_，使用key初始化
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @param key 
 * @param height 
 * @return SkipList<Key, Comparator>::Node* 
 */
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
  list_ = list;
  node_ = nullptr;
}

/**
 * @brief Valid 迭代器是否有效
 * 
 * node_ != nullptr
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @return true 
 * @return false 
 */
template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
  return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
  assert(Valid());
  return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  assert(Valid());
  node_ = list_->FindLessThan(node_->key);
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
  node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

/**
 * @brief 返回一个随机的高度
 * 
 * 从1开始，通过生成随机数，然后以四分之一的概率增加高度，但不能超过最大的高度
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @return int 
 */
template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // Increase height with probability 1 in kBranching
  // 以4分之1的概率增加高度
  static const unsigned int kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && ((rnd_.Next() % kBranching) == 0)) {
    height++;
  }
  assert(height > 0);
  assert(height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
/**
 * @brief KeyIsAfterNode 大于或等于
 * 
 * @param key 
 * @param n 
 * @return true 
 * @return false 
 */
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
  // null n is considered infinite
  return (n != nullptr) && (compare_(n->key, key) < 0);
}

/**
 * @brief 查找key，前置节点将由prev带回
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @param key 
 * @param prev 
 * @return SkipList<Key, Comparator>::Node* 
 */
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) { // key 没有在当前区间
      // Keep searching in this list
      x = next;
    } else {  // key 在当前区间，在低level 继续查找，
      if (prev != nullptr) prev[level] = x;
      if (level == 0) { // 在最低level找到相应位置
        return next; // 可能为nullptr
      } else {
        // Switch to next list
        level--;
      }
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {  // 遇到末尾或者比key大的节点
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      // 查找下一个区间
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast()
    const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

/**
 * @brief Construct a new Skip List< Key,  Comparator>:: Skip List object
 * 
 * 初始化max_height_为1，
 * 分配前置哨兵节点的空间，并初始化
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @param cmp 
 * @param arena 
 */
template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}

/**
 * @brief Insert
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @param key 
 */
template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  // TODO(opt): We can use a barrier-free variant of FindGreaterOrEqual()
  // here since Insert() is externally synchronized.
  // 声明prev节点，用于存储插入位置的前一个节点
  Node* prev[kMaxHeight];
  // 使用FindGreaterOrEqual函数找到第一个大于等于插入key的位置
  Node* x = FindGreaterOrEqual(key, prev);

  // Our data structure does not allow duplicate insertion
  // 不允许插入重复的key
  assert(x == nullptr || !Equal(key, x->key));

  // 使用随机数获取该节点的插入高度
  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    // 大于当前skiplist 最高高度的话，将多出的来的高度的prev 设置为哨兵节点
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // It is ok to mutate max_height_ without any synchronization
    // with concurrent readers.  A concurrent reader that observes
    // the new value of max_height_ will see either the old value of
    // new level pointers from head_ (nullptr), or a new value set in
    // the loop below.  In the former case the reader will
    // immediately drop to the next level since nullptr sorts after all
    // keys.  In the latter case the reader will use the new node.
    // 无需与并发max_height_任何同步即可更改对象。 观察 max_height_ 新值的并发读取器将看到 head_ （nullptr） 
    // 中新级别指针的旧值，或下面的循环中设置的新值。 在上一种情况下，读取器将立即下降到下一个级别，
    // 因为 nullptr 排序时排在所有键之后。 在后一种情况下，读者将使用新节点
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "x" in prev[i].
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    prev[i]->SetNext(i, x);
  }
}

/**
 * @brief Contains 判断skiplist中是否包含key
 * 
 * @tparam Key 
 * @tparam Comparator 
 * @param key 
 * @return true 
 * @return false 
 */
template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  if (x != nullptr && Equal(key, x->key)) {
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SKIPLIST_H_
