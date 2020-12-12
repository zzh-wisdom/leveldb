// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SNAPSHOT_H_
#define STORAGE_LEVELDB_DB_SNAPSHOT_H_

#include "db/dbformat.h"
#include "leveldb/db.h"

namespace leveldb {

class SnapshotList;

// Snapshots are kept in a doubly-linked list in the DB.
// Each SnapshotImpl corresponds to a particular sequence number.
/**
 * @brief SnapshotImpl 快照的实现类
 * 
 * 快照保存在数据库的双向链接列表中。 每个SnapshotImpl对应一个特定的序列号。
 * 
 * 只有一个成员 sequence_number_， 保存该快照的序列号
 * 
 * 生成的快照加入到链表后，在compaction中会根据快照列表中快照的序列号来决定哪些记录不能不被删除。
 * 
 */
class SnapshotImpl : public Snapshot {
 public:
  SnapshotImpl(SequenceNumber sequence_number)
      : sequence_number_(sequence_number) {}

  /**
   * @brief 返回快照的序列号
   * 
   * @return SequenceNumber 
   */
  SequenceNumber sequence_number() const { return sequence_number_; }

 private:
  friend class SnapshotList;

  // SnapshotImpl is kept in a doubly-linked circular list. The SnapshotList
  // implementation operates on the next/previous fields direcly.
  // SnapshotImpl保留在双向链接的循环列表中。
  SnapshotImpl* prev_;
  SnapshotImpl* next_;

  const SequenceNumber sequence_number_;

#if !defined(NDEBUG)
  SnapshotList* list_ = nullptr;   /// 当前快照所在的双向链表
#endif  // !defined(NDEBUG)
};

/**
 * @brief SnapshotList
 * 
 * 快照双向链表，类中只保存了链表的头部节点，该节点是无效的，初始化时里面的prev_和next_指针都指向自身。
 * 因此可以通过判断head.next_是否等于head来判断链表是否为空。
 * 
 * 双向链表中，尾部的元素是最新的，而头部的下一个节点是最旧的。
 * 
 */
class SnapshotList {
 public:
  SnapshotList() : head_(0) {
    head_.prev_ = &head_;
    head_.next_ = &head_;
  }

  bool empty() const { return head_.next_ == &head_; }
  /**
   * @brief 返回最旧的的快照，链表头部的写一个节点
   * 
   * @return SnapshotImpl* 
   */
  SnapshotImpl* oldest() const {
    assert(!empty());
    return head_.next_;
  }
  /**
   * @brief 返回最新的快照，即链表的尾部节点
   * 
   * @return SnapshotImpl* 
   */
  SnapshotImpl* newest() const {
    assert(!empty());
    return head_.prev_;
  }

  // Creates a SnapshotImpl and appends it to the end of the list.
  /**
   * @brief 新创建一个快照
   * 
   * 需要确保最新快照的序列号不大于当前生成快照的序列号。
   * 新生成的快照添加到链表尾部。
   * 
   * @param sequence_number 
   * @return SnapshotImpl* 
   */
  SnapshotImpl* New(SequenceNumber sequence_number) {
    assert(empty() || newest()->sequence_number_ <= sequence_number);

    SnapshotImpl* snapshot = new SnapshotImpl(sequence_number);

#if !defined(NDEBUG)
    snapshot->list_ = this;
#endif  // !defined(NDEBUG)
    snapshot->next_ = &head_;
    snapshot->prev_ = head_.prev_;
    snapshot->prev_->next_ = snapshot;
    snapshot->next_->prev_ = snapshot;
    return snapshot;
  }

  // Removes a SnapshotImpl from this list.
  //
  // The snapshot must have been created by calling New() on this list.
  //
  // The snapshot pointer should not be const, because its memory is
  // deallocated. However, that would force us to change DB::ReleaseSnapshot(),
  // which is in the API, and currently takes a const Snapshot.
  /**
   * @brief 从列表中删除快照。
   * 
   * 快照必须是通过在此列表中调用 New（） 创建的。
   * 
   * 指针snapshot指向的内存不应该是const，因为我们将要释放它
   * 
   * @param snapshot 
   */
  void Delete(const SnapshotImpl* snapshot) {
#if !defined(NDEBUG)
    assert(snapshot->list_ == this);
#endif  // !defined(NDEBUG)
    snapshot->prev_->next_ = snapshot->next_;
    snapshot->next_->prev_ = snapshot->prev_;
    delete snapshot;
  }

 private:
  // Dummy head of doubly-linked list of snapshots
  SnapshotImpl head_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SNAPSHOT_H_
