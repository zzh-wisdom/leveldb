// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <cassert>

#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == nullptr
                         ? nullptr
                         : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }

  Options options;                      /// data_block的选项
  Options index_block_options;          /// index block的选项, index_block_options.block_restart_interval = 1;即key不共用前缀
  WritableFile* file;                   /// sstable文件
  uint64_t offset;                      /// 要写入data block在sstable文件中的偏移，初始0
  Status status;                        /// 当前状态, 初始ok
  BlockBuilder data_block;              /// 当前操作的data block
  BlockBuilder index_block;             /// sstable的index block
  std::string last_key;                 /// 当前data block最后的k/v对的key
  int64_t num_entries;                  /// kv数量
  bool closed;                          /// Either Finish() or Abandon() has been called.
  FilterBlockBuilder* filter_block;     /// 过滤器块数据，即Meta Block

  /// We do not emit the index entry for a block until we have seen the
  /// first key for the next data block.  This allows us to use shorter
  /// keys in the index block.  For example, consider a block boundary
  /// between the keys "the quick brown fox" and "the who".  We can use
  /// "the r" as the key for the index block entry since it is >= all
  /// entries in the first block and < all entries in subsequent
  /// blocks.
  ///
  /// @invariant r->pending_index_entry is true only if data_block is empty.
  bool pending_index_entry;
  BlockHandle pending_handle;  /// Handle to add to index block

  std::string compressed_output;  ///压缩后的data block，临时存储，写入后即被清空
};

/**
 * @brief Construct a new Table Builder:: Table Builder object
 * 
 * 调用 filter_block 的 StartBlock 方法
 * 
 * @param options 
 * @param file 
 */
TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

/**
 * @brief 改变选项
 * 
 * 能改变的选项有：
 * - data block 的 block_restart_interval
 * 
 * @param options 
 * @return Status 
 */
Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  // 
  // 注意 data_block 和 index_block（BlockBuilders）的 Options 分别指向下面这两个Option实例，所以就附带更新了
  rep_->options = options;
  rep_->index_block_options = options;
  // 确保 block_restart_interval 为1，不被改变
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

/**
 * @brief add kv
 * 
 * 1. 首先保证文件没有close，也就是没有调用过Finish/Abandon，以及保证当前status是ok的；如果当前有缓存的kv对，保证新加入的key是最大的。
 * 2. 如果标记r->pending_index_entry为true，表明遇到下一个data block的第一个k/v，根据key调整r->last_key，
 *    这是通过Comparator的FindShortestSeparator完成的。然后向index_block添加一条entry，最后将r->pending_index_entry设置为false。
 * 3. 如果filter_block不为空，就把key加入到filter_block中。
 * 4. 设置r->last_key = key，将(key, value)添加到r->data_block中，并更新entry数。
 * 5. 如果data block的大小超过限制，就立刻Flush到文件中。
 * 
 * \note index block的entry中，key是大于等于对应data block的最后一个key的字符串，而handle却是指向该data block的起始位置
 * 
 * @param key 
 * @param value 
 */
void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  if (r->pending_index_entry) {
    // 只有flush后，才将pending_index_entry置为true，所以index entry是在每一块flush，且下一块的第一个kv到来时，才写入的
    assert(r->data_block.empty());
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(key);
  }

  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);

  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
}

/**
 * @brief 
 * 
 * 1. 确保rep未closed，当前data_block为空，r->pending_index_entry=false
 * 2. 将block数据写入，并更新 pending_handle
 * 3. 写入成功后，将pending_index_entry置为true，然后调用file的flush函数，数据落盘
 * 4. 如果filter_block不等nullptr，根据 r->offset StartBlock
 * 
 */
void TableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    r->pending_index_entry = true;
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
}

/**
 * @brief 将data block写入文件，同时还设置data block的index entry信息(BlockHandle)
 * 
 * 写 block 数据时，在尾部额外添加type和crc信息(WriteRawBlock函数中实现)
 * 
 * 写完成后，将block reset
 * 
 * @param block 
 * @param handle 
 */
void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?
  switch (type) {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: {
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) &&
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else {
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        // 备份不超过 12.5%，则直接存储原始数据
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

/**
 * @brief 写原生block数据到file，在block尾部添加type和crc
 * 
 * 将handle置为写前的 r->offset 和原生block的大小，
 * 写入后，再更新 r->offset
 * 
 * @param block_contents 
 * @param type  压缩的类型
 * @param handle 
 */
void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type, BlockHandle* handle) {
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer + 1, crc32c::Mask(crc));
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const { return rep_->status; }

/**
 * @brief 调用Finish函数，表明调用者将所有已经添加的k/v对持久化到sstable，并关闭sstable文件。
 * 
 * 1. 调用Flush写入最后一块(如果有), 然后设置关闭标志closed=true。表明该sstable已经关闭，不能再添加k/v对。
 * 2. 将（一个）filter block写入到文件（不压缩的方式），同时带回filter_block_handle
 * 3. 将一条entry（从"filter.Name"到filter data位置的映射）添加到meta index block，然后写入文件。
 * 4. 写入index block，如果成功Flush过data block，那么需要为最后一块data block设置index entry，并加入到index block中。
 * 5. 写入Footer
 * 
 * @return Status 
 */
Status TableBuilder::Finish() {
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // Write metaindex block
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != nullptr) {
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // Write index block
  if (ok()) {
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

/**
 * @brief 放弃构建table
 * 
 * 简单将closed设置为true
 * 
 */
void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const { return rep_->num_entries; }

uint64_t TableBuilder::FileSize() const { return rep_->offset; }

}  // namespace leveldb
