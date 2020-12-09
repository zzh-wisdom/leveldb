// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/table_cache.h"

#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "util/coding.h"

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

/**
 * @brief 为LRUCache注册的删除函数DeleteEntry。
 * 
 * 删除的value的类型为 TableAndFile
 * 
 * @param key 
 * @param value 
 */
static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

/**
 * @brief 为Iterator注册的清除函数UnrefEntry
 * 
 * @param arg1 
 * @param arg2 
 */
static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname, const Options& options,
                       int entries)
    : env_(options.env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {}

TableCache::~TableCache() { delete cache_; }

/**
 * @brief 查找file对应的Cache的Handle。
 * 
 * 如果在Cache中没有找到对应的Handle，则从文件中读取内容到Cache中
 *  
 *  > SSTable数据文件有两种后缀名格式：
 *  > - ldb
 *  > - sst
 *  > 按照上面的顺序依次打开，直到有一个成功
 * 
 * 1.  首先根据file number从cache中查找table，找到就直接返回成功。
 * 2. 如果没有找到，说明table不在cache中，则根据file number和db name打开一个RadomAccessFile。
 *    Table文件格式为：<db name>.<filenumber(%6u)>.sst。如果文件打开成功，则调用Table::Open读取sstable文件。
 * 3. 如果Table::Open成功则，插入到Cache中。如果失败，则删除file，直接返回失败，失败的结果是不会cache的。
 * 
 * @param file_number 
 * @param file_size 
 * @param handle 
 * @return Status 
 */
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == nullptr) {
    std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = nullptr;
    Table* table = nullptr;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) {
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
      s = Table::Open(options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == nullptr);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

/**
 * @brief 该函数为指定的file返回一个iterator
 * 
 * 对应的文件长度必须是"file_size"字节,
 * 如果tableptr不是NULL，那么*tableptr保存的是底层的Table指针。
 * 返回的*tableptr是cache拥有的，不能被删除，生命周期同返回的iterator
 * 
 * @param options 
 * @param file_number 
 * @param file_size 
 * @param tableptr 
 * @return Iterator* 
 */
Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number, uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != nullptr) {
    *tableptr = nullptr;
  }

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }

  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  Iterator* result = table->NewIterator(options);
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != nullptr) {
    *tableptr = table;
  }
  return result;
}

/**
 * @brief 从TableCache中查找
 * 
 * 如果在指定文件中seek 到internal key "k" 找到一个entry，就调用 (*handle_result)(arg,found_key, found_value).
 * 
 * 1. 根据 file_number 和 file_size 查找对应 Cache 的 Handle
 * 2. 根据handle得到对应的Table
 * 3. 在Table中查找key（InternalGet）
 * 4. cache_->Release(handle);
 * 
 * 注意：这里返回的Status只能只是查找的过程中是否出现错误，而是否找到，需要根据参数arg带回的结果来决定
 * 
 * @param options 
 * @param file_number SSTable文件编号
 * @param file_size SSTable文件大小
 * @param k 查找的Internal-key
 * @param arg Saver
 * @param handle_result 将查询结果存储到 arg 中的处理函数
 * @return Status 
 */
Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&,
                                             const Slice&)) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, handle_result);
    cache_->Release(handle);
  }
  return s;
}

/**
 * @brief 该函数用以清除指定文件所有cache的entry
 * 
 * 函数实现很简单，就是根据file number清除cache对象。
 * 
 * @param file_number 
 */
void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
