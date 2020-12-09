// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace leveldb {

struct Table::Rep {
  ~Rep() {
    delete filter;
    delete[] filter_data;
    delete index_block;
  }

  Options options;
  Status status;
  RandomAccessFile* file;
  uint64_t cache_id;             /// block cache的ID，用于组建block cache结点的key
  FilterBlockReader* filter;
  const char* filter_data;

  BlockHandle metaindex_handle;  /// Handle to metaindex_block: saved from footer
  Block* index_block;
};

/**
 * @brief 从RandomAccessFile中打开，构建Table实例(解析一个.sst文件，也有可能是.ldb)
 * 
 * 1. 首先从文件的结尾读取Footer，并Decode到Footer对象中，如果文件长度小于Footer的长度(48bytes)，则报错
 * 2. 根据Footer中的index_handle，读取index_block的内容
 * 3. 成功读取了footer和index block，此时table已经可以响应请求了。构建Table对象，
 *    并读取metaindex数据构建filter policy。如果option打开了cache，还要为table创建cache_id。
 * 
 * @param options 
 * @param file 
 * @param size 
 * @param table 
 * @return Status 
 */
Status Table::Open(const Options& options, RandomAccessFile* file,
                   uint64_t size, Table** table) {
  *table = nullptr;
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Read the index block
  BlockContents index_block_contents;
  ReadOptions opt;
  if (options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    Block* index_block = new Block(index_block_contents);
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = nullptr;
    rep->filter = nullptr;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);
  }

  return s;
}

/**
 * @brief 根据Footer读取IndexMeta Block，再读取Meta block数据（filter）。
 * 
 * 1. 判断，若options.filter_policy==nullptr，则不需要filter
 * 2. 调用ReadBlock，读取Meta block数据（即时出错了，也不要紧，直接返回）
 * 3. 根据读取的content构建Block，找到指定的filter；如果找到了就调用ReadFilter构建filter对象
 * 
 * @param footer 
 */
void Table::ReadMeta(const Footer& footer) {
  if (rep_->options.filter_policy == nullptr) {
    return;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  Block* meta = new Block(contents);

  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());
  }
  delete iter;
  delete meta;
}

/**
 * @brief 读取Meta Block（Filter Block）
 * 
 * 1. 从 slice中解码得到filter_handle
 * 2. 根据handle读取MetaBlock
 * 3. 根据读取的结果（block.heap_allocated），设置rep_->filter_data指针，以便后续释放空间
 * 4. 构建FilterBlockReader，放到rep_->filter
 * 
 * @param filter_handle_value 
 */
void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // We might want to unify with ReadBlock() if we start
  // requiring checksum verification in Table::Open.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents block;
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  if (block.heap_allocated) {
    rep_->filter_data = block.data.data();  // Will need to delete later
  }
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() { delete rep_; }

/**
 * @brief delete block
 * 
 * @param arg  转化为Block*
 * @param ignored 
 */
static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}

/**
 * @brief delete block
 * 
 * 以便当block需要从Cache中移除时，释放block的空间
 * 
 * @param key 
 * @param value  转化为Block*
 */
static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

/**
 * @brief 释放Cache::Handle
 * 
 * 引用计数减一，减到1或者0会做相应的处理
 * 
 * @param arg 
 * @param h 
 */
static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

/**
 * @brief 
 * 
 * 我们已经知道各种Block的存储格式都是相同的，但是各自block data存储的k/v又互不相同，
 * 于是我们就需要一个途径，能够在使用同一个方式遍历不同的block时，又能解析这些k/v。
 * 这就是BlockFunction，它又返回了一个针对block data的Iterator
 * 
 * Convert an index iterator value (i.e., an encoded BlockHandle)
 * into an iterator over the contents of the corresponding block.
 * 
 * 这其中还用到了LRUCache（在options中）。
 * 
 * 1. 从参数中解析出BlockHandle对象，其中arg就是Table对象，index_value存储的是BlockHandle对象，读取Block的索引。
 * 2. 根据block handle，首先尝试从cache中直接取出block，不在cache中则调用ReadBlock从文件读取，读取成功后，根据option尝试将block加入到LRU cache中，
 *    并在Insert到Cache的时候注册了释放函数DeleteCachedBlock。
 * 3. 如果读取到了block，调用Block::NewIterator接口创建Iterator，如果cache handle为NULL，则注册DeleteBlock，否则注册ReleaseBlock，事后清理。
 * 
 * @param arg 
 * @param options 
 * @param index_value  一个handle，根据该参数从table中读取block
 * @return Iterator* 
 */
Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != nullptr) {
      char cache_key_buffer[16];  // cache_id + handle.offset
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer + 8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(key, block, block->size(),
                                               &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != nullptr) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == nullptr) {
      iter->RegisterCleanup(&DeleteBlock, block, nullptr);
    } else {
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}
/**
 * @brief 创建 Table 的迭代器
 * 
 * 通过Iterator对象，调用者就可以遍历Table的内容，它简单的返回了一个TwoLevelIterator对象。
 * 
 * @param options 
 * @return Iterator* 
 */
Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

/**
 * @brief 查找InternalKey的方法，使用了filter
 * 
 * 私有函数。在找到数据后，就调用传入的函数指针handle_result执行调用者的自定义处理逻辑，并且TableCache可能会做缓存。
 * 
 * 1. 首先根据传入的k定位数据，这需要indexblock的Iterator
 * 2. 如果key是合法的，取出filter。如果table使用了filter，则检查key是否存在，这可以快速判断，提升效率。
 * 3. 如果可能存在，则调用BlockReader获取block的迭代器，然后进行查找，对结果调用传入的函数handle_result进行处理
 * 4. 最后返回结果，删除临时迭代器。
 * 
 * @param options 
 * @param k  查找的InternalKey
 * @param arg 
 * @param handle_result 
 * @return Status 
 */
Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&,
                                                const Slice&)) {
  Status s;
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    if (filter != nullptr && handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // Not found
    } else {
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        (*handle_result)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

/**
 * @brief 近似定位key对应的value的offset
 * 
 * 这里并不是精确的定位，而是在Table中找到第一个>=指定key的k/v对，然后返回其value在sstable文件中的偏移
 * 
 * @param key 
 * @return uint64_t 
 */
uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      // 无法解码handle，只返回metaindex块的偏移量，此时offset接近整个文件的大小
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    // 键在文件的最后一个键之后。通过返回metaindex块的偏移量（在文件末尾附近）来近似估算偏移量。
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
