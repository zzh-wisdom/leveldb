// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/cache.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "port/port.h"
#include "port/thread_annotations.h"
#include "util/hash.h"
#include "util/mutexlock.h"

/**
 * @file cache.cc
 * 
 * Table Cache 实现
 * 
 */

namespace leveldb {

Cache::~Cache() {}

/**
 * @namespace Cache
 * 
 */
namespace {

// LRU cache implementation
//
// Cache entries have an "in_cache" boolean indicating whether the cache has a
// reference on the entry.  The only ways that this can become false without the
// entry being passed to its "deleter" are via Erase(), via Insert() when
// an element with a duplicate key is inserted, or on destruction of the cache.
//
// The cache keeps two linked lists of items in the cache.  All items in the
// cache are in one list or the other, and never both.  Items still referenced
// by clients but erased from the cache are in neither list.  The lists are:
// - in-use:  contains the items currently referenced by clients, in no
//   particular order.  (This list is used for invariant checking.  If we
//   removed the check, elements that would otherwise be on this list could be
//   left as disconnected singleton lists.)
// - LRU:  contains the items not currently referenced by clients, in LRU order
// Elements are moved between these lists by the Ref() and Unref() methods,
// when they detect an element in the cache acquiring or losing its only
// external reference.

// An entry is a variable length heap-allocated structure.  Entries
// are kept in a circular doubly linked list ordered by access time.

/**
 * @brief LRUHandle就是Cache中的一个节点
 * 
 * 这个结构体设计的巧妙之处在于，它既可以作为HashTable中的结点，也可以作为LRUCache中的结点。
 * 
 */
struct LRUHandle {
  void* value;
  void (*deleter)(const Slice&, void* value); /// 删除器。当refs == 0 时，调用deleter完成value对象释放。
  LRUHandle* next_hash; /// 作为HashTable中的节点，指向hash值相同的节点（解决hash冲突采用链地址法）
  LRUHandle* next;      /// 作为LRUCache中的节点，指向后继
  LRUHandle* prev;      /// 作为LRUCache中的节点，指向前驱
  size_t charge;  // TODO(opt): Only allow uint32_t? 用户指定占用缓存的大小
  size_t key_length; /// key长度
  bool in_cache;     /// Whether entry is in the cache.
  uint32_t refs;     /// References, including cache reference, if present.
  uint32_t hash;     /// Hash of key(); used for fast sharding and comparisons，便于更快速的比较和扩容
  char key_data[1];  /// Beginning of key， key_data位于结构体的最后面，可在申请内存时，申请足够多的空间。 

  /**
   * @brief 获取key的切片
   * 
   * @return Slice 
   */
  Slice key() const {
    // next_ is only equal to this if the LRU handle is the list head of an
    // empty list. List heads never have meaningful keys.
    assert(next != this);

    return Slice(key_data, key_length);
  }
};

// We provide our own simple hash table since it removes a whole bunch
// of porting hacks and is also faster than some of the built-in hash
// table implementations in some of the compiler/runtime combinations
// we have tested.  E.g., readrandom speeds up by ~5% over the g++
// 4.4.3's builtin hashtable.
/**
 * @brief hash table
 * 
 * 我们提供了自己的简单哈希表，因为它消除了一大堆移植技巧，并且比我们测试过的某些编译器/运行时组合中的某些内置哈希表实现要快。 
 * 例如，与g ++ 4.4.3的内置哈希表相比，readrandom的速度提高了5％。
 * 
 * 与常见的实现一样，hash table由一系列存储桶组成（数组），然后每个桶使用链表来解决hash冲突
 * 
 * @attention HandleTable 类中的函数都只处理LRUHandle::next_hash指针，
 * 即节点它所在的冲突链，next和pre没有做任何处理，应该是外面再封装的LRUCache
 * 来处理。形成两条链（in-use和LRU）
 * 
 */
class HandleTable {
 public:
  HandleTable() : length_(0), elems_(0), list_(nullptr) { Resize(); }
  ~HandleTable() { delete[] list_; }

  /**
   * @brief 查找key和hash对应的节点LRUHandle
   * 
   * @param key 
   * @param hash 
   * @return LRUHandle* 存在则返回nullptr
   */
  LRUHandle* Lookup(const Slice& key, uint32_t hash) {
    return *FindPointer(key, hash);
  }

  /**
   * @brief 插入一个节点LRUHandle
   * 
   * @attention  插入的h并没有处理next和pre
   * 
   * @param h 
   * @return LRUHandle*  返回被替换的handle(如果存在)
   */
  LRUHandle* Insert(LRUHandle* h) {
    LRUHandle** ptr = FindPointer(h->key(), h->hash);
    LRUHandle* old = *ptr;
    h->next_hash = (old == nullptr ? nullptr : old->next_hash);
    *ptr = h;
    // 不管找没找到h结点，都是可以直接将h替换到*ptr的。
    // 1.如果找到了，因为key相同，直接替换相当于替换结点中的value
    // 2.如果没找到，直接替换是理所当然的了
    if (old == nullptr) {
      ++elems_;
      if (elems_ > length_) {
        // Since each cache entry is fairly large, we aim for a small
        // average linked list length (<= 1).
        // 保证链表的平均长度小于等于1
        Resize();
      }
    }
    // 将old返回很重要，因为这个被摘到的handle需要在函数外面销毁。
    return old;
  }

  /**
   * @brief 删除节点
   * 
   * @attention 没有处理next和pre
   * 
   * @param key 
   * @param hash 
   * @return LRUHandle* 返回需要被删除的节点(如果存在)
   */
  LRUHandle* Remove(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = FindPointer(key, hash);
    LRUHandle* result = *ptr;
    if (result != nullptr) {
      *ptr = result->next_hash;
      --elems_;
    }
    return result;
  }

 private:
  /// The table consists of an array of buckets where each bucket is
  /// a linked list of cache entries that hash into the bucket.
  uint32_t length_;   /// 哈希地址数组的长度
  uint32_t elems_;    /// 哈希表中所有结点的总数
  LRUHandle** list_;  /// 数组链表

  /// Return a pointer to slot that points to a cache entry that
  /// matches key/hash.  If there is no such cache entry, return a
  /// pointer to the trailing slot in the corresponding linked list.
  /**
   * @brief 寻找插槽指针
   * 
   * - 如果找到了结点，返回该结点的二级指针。
   * - 如果没找到结点，分两种情况:
   * 1. 根据哈希值求出的哈希地址是空的，也就是说以该哈希地址（哈希桶）为头的单链表
   *    还没有结点。hash & (length_ - 1)的取值范围是0—（length_-1）。此时返回
   *    哈希地址的二级指针，*ptr=NULL 。  
   *  2. 查找到了以某哈希地址为头的单链表的尾部，也没找到该结点。此时返回
   *    尾部结点next_hash域的二级指针，*ptr=NULL 。
   * 
   * @param key 
   * @param hash 
   * @return LRUHandle** 
   */
  LRUHandle** FindPointer(const Slice& key, uint32_t hash) {
    LRUHandle** ptr = &list_[hash & (length_ - 1)];
    while (*ptr != nullptr && ((*ptr)->hash != hash || key != (*ptr)->key())) {
      ptr = &(*ptr)->next_hash;
    }
    return ptr;
  }

  /**
   * @brief 哈希表（内置数组）扩容
   * 
   * HandleTable内部维护了一个LRUHandle*的数组，默认大小为4。随着插入数据的增多，
   * 该数组会自动增长一倍，并将原数组中的数据重新分配到新的数组中。
   * 
   */
  void Resize() {
    uint32_t new_length = 4;
    // 倍数扩展，直到大于等于元素的个数
    while (new_length < elems_) {
      new_length *= 2;
    }
    // 申请新数组
    LRUHandle** new_list = new LRUHandle*[new_length];
    memset(new_list, 0, sizeof(new_list[0]) * new_length);
    uint32_t count = 0;
    // 重新分桶，hash & (new_length - 1)
    for (uint32_t i = 0; i < length_; i++) {
      LRUHandle* h = list_[i];
      while (h != nullptr) {
        LRUHandle* next = h->next_hash;
        uint32_t hash = h->hash;
        LRUHandle** ptr = &new_list[hash & (new_length - 1)];
        h->next_hash = *ptr;  // 头插法
        *ptr = h;
        h = next;  // 用next来遍历数组
        count++;
      }
    }
    assert(elems_ == count);
    // 删除旧数组，赋值新数组
    delete[] list_;
    list_ = new_list;
    length_ = new_length;
  }
};

// A single shard of sharded cache.
/**
 * @brief LRU cache implementation 分片缓存(sharded cache)的单个分片。
 * 
 * Cache条目具有“in_cache”的布尔值，指示Cache是否在条目上具有引用。
 * 在没有将条目传递给它的“deleter”的情况下，变为false的唯一方法是通过Erase（）、
 * 通过Insert（）插入具有重复键的元素或销毁Cache。
 * 
 * Cache中含有两条链表。一个item同时只能处于一条链表中。
 * 用户仍引用但从缓存中删除的item均不在两个链表中。这两条链表分别为：
 * - in-use：包含用户当前引用的项目，不分先后顺序。
 * - LRU：包含客户端当前未引用的项（按LRU顺序）
 * 
 * 当调用Ref（）和Unref（）函数时，如果元素获得或者失去它的唯一引用，那么元素将在这两个链表之间移动。
 * 
 * 条目是可变长度的堆分配结构。条目保存在按访问时间排序的循环双向链表中。
 * 
 * @attention 其中的capacity的单位不一定是字节，也可以是个数，实际看用户怎么操作。
 * - 在table cache的使用中，就是以个数为单位，最多缓存1000个tableindex数据。
 * - 在block cache的使用中，则是以字节为单位，8M
 * 
 */
class LRUCache {
 public:
  LRUCache();
  ~LRUCache();

  // Separate from constructor so caller can easily make an array of LRUCache
  /// 与构造函数分离，因此调用者可以轻松地创建LRUCache数组
  void SetCapacity(size_t capacity) { capacity_ = capacity; }

  /// Like Cache methods, but with an extra "hash" parameter.
  Cache::Handle* Insert(const Slice& key, uint32_t hash, void* value,
                        size_t charge,
                        void (*deleter)(const Slice& key, void* value));
  Cache::Handle* Lookup(const Slice& key, uint32_t hash);
  void Release(Cache::Handle* handle);
  void Erase(const Slice& key, uint32_t hash);
  void Prune();
  /**
   * @brief 返回空间总使用量
   * 
   * @return size_t 
   */
  size_t TotalCharge() const {
    MutexLock l(&mutex_);
    return usage_;
  }

 private:
  void LRU_Remove(LRUHandle* e);
  void LRU_Append(LRUHandle* list, LRUHandle* e);
  void Ref(LRUHandle* e);
  void Unref(LRUHandle* e);
  bool FinishErase(LRUHandle* e) EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  /// 容量，Initialized before use. 用于判断是否需要回收
  size_t capacity_;

  /// mutex_ protects the following state(除了capacity_). 
  mutable port::Mutex mutex_;
  /// 内存使用量，用于判断是否需要回收
  size_t usage_ GUARDED_BY(mutex_);

  // Dummy head of LRU list.
  // lru.prev is newest entry, lru.next is oldest entry.
  // Entries have refs==1 and in_cache==true.
  /// LRU列表的虚拟头。双向循环链表
  /// lru.prev 是最新的元素, lru.next 是最旧的元素.
  /// 元素的refs==1且 in_cache==true（只有cache在引用元素）.
  LRUHandle lru_ GUARDED_BY(mutex_);

  // Dummy head of in-use list.
  // Entries are in use by clients, and have refs >= 2 and in_cache==true.
  /// in-use 链表的虚拟头部，双向循环链表（也是lru方式）
  /// 存放客户端使用的元素，refs >= 2 且 in_cache==true.
  LRUHandle in_use_ GUARDED_BY(mutex_);

  /// hash桶，真正存放数据的地方
  HandleTable table_ GUARDED_BY(mutex_);
};

/**
 * @brief Construct a new LRUCache::LRUCache object
 * 
 * 将链表lru_和in_use_初始化为指向自身的空循环链表
 * 
 */
LRUCache::LRUCache() : capacity_(0), usage_(0) {
  // Make empty circular linked lists.
  lru_.next = &lru_;
  lru_.prev = &lru_;
  in_use_.next = &in_use_;
  in_use_.prev = &in_use_;
}

/**
 * @brief Destroy the LRUCache::LRUCache object
 * 
 * 调用析构函数时，in_use_链表必须为空assert(in_use_.next == &in_use_)
 * 
 * 销毁lru_链表
 * 
 */
LRUCache::~LRUCache() {
  assert(in_use_.next == &in_use_);  // Error if caller has an unreleased handle
  for (LRUHandle* e = lru_.next; e != &lru_;) {
    LRUHandle* next = e->next;
    assert(e->in_cache);
    e->in_cache = false;
    assert(e->refs == 1);  // Invariant of lru_ list.
    Unref(e);
    e = next;
  }
}

/**
 * @brief 将LRUHandle的引用计数加一
 * 
 * 如果原引用计数为1，还需要添加到in_use_链表
 * 
 * @param e 
 */
void LRUCache::Ref(LRUHandle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    LRU_Remove(e);
    LRU_Append(&in_use_, e);
  }
  e->refs++;
}

/**
 * @brief 将LRUHandle的引用计数减一
 * 
 * - 引用计数为0，则需要回收
 * - 引用计数为1，则从in_use_链表删除，重新加入到lru链表尾部等待被淘汰
 * 
 * @param e 
 */
void LRUCache::Unref(LRUHandle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    (*e->deleter)(e->key(), e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    LRU_Remove(e);
    LRU_Append(&lru_, e);
  }
}

/**
 * @brief 从链表中删除节点
 * 
 * @param e 
 */
void LRUCache::LRU_Remove(LRUHandle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
}

/**
 * @brief 将节点添加到链表的尾部（即时最新的）
 * 
 * @todo
 * 对于链表的插入，可以看到，都是先赋值e的指针，然后再通过e找到它的前驱和后继节点，
 * 在相应修改它们的指针，使用list不是更直接吗。
 * ```cpp
 * e->next = list;
 * e->prev = list->prev;
 * e->prev->next = e;
 * e->next->prev = e;
 * ```
 * 
 * @param list 
 * @param e 
 */
void LRUCache::LRU_Append(LRUHandle* list, LRUHandle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

Cache::Handle* LRUCache::Lookup(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  LRUHandle* e = table_.Lookup(key, hash);
  if (e != nullptr) {
    Ref(e);
  }
  return reinterpret_cast<Cache::Handle*>(e);
}

/**
 * @brief 释放元素
 * 
 * 线程安全（加锁）
 * 引用计数减一 Unref()
 * 
 * @param handle 
 */
void LRUCache::Release(Cache::Handle* handle) {
  MutexLock l(&mutex_);
  Unref(reinterpret_cast<LRUHandle*>(handle));
}

/**
 * @brief 插入元素
 * 
 * @param key 
 * @param hash 
 * @param value 
 * @param charge 
 * @param deleter 
 * @return Cache::Handle*  返回插入的句柄
 */
Cache::Handle* LRUCache::Insert(const Slice& key, uint32_t hash, void* value,
                                size_t charge,
                                void (*deleter)(const Slice& key,
                                                void* value)) {
  MutexLock l(&mutex_);

  // 注意在使用malloc申请空间时，sizeof(LRUHandle)-1，其中减去的1就是key_data[1]
  LRUHandle* e =
      reinterpret_cast<LRUHandle*>(malloc(sizeof(LRUHandle) - 1 + key.size()));  // 末尾的空间用来存放key
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key.size();
  e->hash = hash;
  e->in_cache = false;
  e->refs = 1;  // for the returned handle.
  std::memcpy(e->key_data, key.data(), key.size());

  if (capacity_ > 0) {
    e->refs++;  // for the cache's reference.
    e->in_cache = true;
    LRU_Append(&in_use_, e);
    usage_ += charge;
    FinishErase(table_.Insert(e));   // 擦除被删除的节点
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.) 关闭cache
    // next is read by key() in an assert, so it must be initialized
    e->next = nullptr;
  }
  // 回收空间，空间使用量已经超过容量限定
  // 回收最旧的节点，直到空间使用量小于容量capacity_
  // 实际上，如果大量节点都在被使用，还是会占用大量内存的
  while (usage_ > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    assert(old->refs == 1);
    bool erased = FinishErase(table_.Remove(old->key(), old->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }

  return reinterpret_cast<Cache::Handle*>(e);
}

/**
 * @brief 擦除指定的节点LRUHandle(已经从HandleTable中删除)
 * 
 * If e != nullptr, finish removing *e from the cache; it has already been
 * removed from the hash table.  Return whether e != nullptr.
 * 
 * @param e 
 * @return true  e == nullptr
 * @return false e != nullptr
 */
bool LRUCache::FinishErase(LRUHandle* e) {
  if (e != nullptr) {
    assert(e->in_cache);
    LRU_Remove(e);
    e->in_cache = false;
    usage_ -= e->charge;
    Unref(e);
  }
  return e != nullptr;
}

/**
 * @brief 擦除key和hash对应的节点（线程安全）
 * 
 * @param key 
 * @param hash 
 */
void LRUCache::Erase(const Slice& key, uint32_t hash) {
  MutexLock l(&mutex_);
  FinishErase(table_.Remove(key, hash));
}

/**
 * @brief 清空lru链表
 * 
 */
void LRUCache::Prune() {
  MutexLock l(&mutex_);
  while (lru_.next != &lru_) {
    LRUHandle* e = lru_.next;
    assert(e->refs == 1);
    bool erased = FinishErase(table_.Remove(e->key(), e->hash));
    if (!erased) {  // to avoid unused variable when compiled NDEBUG
      assert(erased);
    }
  }
}

static const int kNumShardBits = 4;
/**
 * @brief 分片的个数
 * 
 */
static const int kNumShards = 1 << kNumShardBits;

class ShardedLRUCache : public Cache {
 private:
  LRUCache shard_[kNumShards];  // 分片数组，16片LRUCache缓存
  port::Mutex id_mutex_;
  uint64_t last_id_;     /// 用于生成ID

  /**
   * @brief 根据切片Slice生成hash值
   * 
   * @param s 
   * @return uint32_t 
   */
  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  /**
   * @brief 根据hash值得到“分片”的值
   * 
   * 取哈希值的高四位来定位使用那片LRUCache缓存
   * hash >> (32 - kNumShardBits)
   * 
   * @param hash 
   * @return uint32_t 
   */
  static uint32_t Shard(uint32_t hash) { return hash >> (32 - kNumShardBits); }

 public:
  /**
   * @brief Construct a new ShardedLRUCache object
   * 
   * @param capacity 容量
   */
  explicit ShardedLRUCache(size_t capacity) : last_id_(0) {
    const size_t per_shard = (capacity + (kNumShards - 1)) / kNumShards;
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].SetCapacity(per_shard);
    }
  }
  ~ShardedLRUCache() override {}
  /**
   * @brief 插入元素
   * 
   * @param key 
   * @param value 
   * @param charge 
   * @param deleter 
   * @return Handle* 
   */
  Handle* Insert(const Slice& key, void* value, size_t charge,
                 void (*deleter)(const Slice& key, void* value)) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Insert(key, hash, value, charge, deleter);
  }

  /**
   * @brief 查找key的缓存节点的Handle
   * 
   * @param key 
   * @return Handle* 
   */
  Handle* Lookup(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    return shard_[Shard(hash)].Lookup(key, hash);
  }
  
  /**
   * @brief 释放指定节点
   * 
   * @param handle 
   */
  void Release(Handle* handle) override {
    LRUHandle* h = reinterpret_cast<LRUHandle*>(handle);
    shard_[Shard(h->hash)].Release(handle);
  }

  /**
   * @brief 擦除指定节点
   * 
   * @param key 
   */
  void Erase(const Slice& key) override {
    const uint32_t hash = HashSlice(key);
    shard_[Shard(hash)].Erase(key, hash);
  }

  /**
   * @brief 获取Handle中的Value指针
   * 
   * @param handle 
   * @return void* 
   */
  void* Value(Handle* handle) override {
    return reinterpret_cast<LRUHandle*>(handle)->value;
  }

  /**
   * @brief 生成一个新的id，用于处理多线程同时访问缓存时的同步，用于Block Cache
   * 
   * @return uint64_t 
   */
  uint64_t NewId() override {
    MutexLock l(&id_mutex_);
    return ++(last_id_);
  }

  /**
   * @brief 清除所有分片的lru链表
   * 
   */
  void Prune() override {
    for (int s = 0; s < kNumShards; s++) {
      shard_[s].Prune();
    }
  }

  /**
   * @brief 计算总的空间占用
   * 
   * @return size_t 
   */
  size_t TotalCharge() const override {
    size_t total = 0;
    for (int s = 0; s < kNumShards; s++) {
      total += shard_[s].TotalCharge();
    }
    return total;
  }
};

}  // end anonymous namespace

/**
 * @brief 新建一个ShardedLRUCache实例
 * 
 * @param capacity 
 * @return Cache* 
 */
Cache* NewLRUCache(size_t capacity) { return new ShardedLRUCache(capacity); }

}  // namespace leveldb
