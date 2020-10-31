// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_FORMAT_H_
#define STORAGE_LEVELDB_TABLE_FORMAT_H_

#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/table_builder.h"

namespace leveldb {

class Block;
class RandomAccessFile;
struct ReadOptions;

// BlockHandle is a pointer to the extent of a file that stores a data
// block or a meta block.
/**
 * \details BlockHandle是指向存储data块或meta块的文件范围的指针。
 * 
 */
class BlockHandle {
 public:
  /// Maximum encoding length of a BlockHandle
  /// 一个 BlockHandle 最大可以编码成20个字节，这是因为采用了压缩(PutVarint64)的编码方案
  enum { kMaxEncodedLength = 10 + 10 };

  BlockHandle();

  // The offset of the block in the file.
  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  // The size of the stored block
  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_;
  uint64_t size_;
};

// Footer encapsulates the fixed information stored at the tail
// end of every table file.
/**
 * @brief 页脚Footer封装了存储在每个表文件末尾的固定信息。
 * 
 * 一个48个字节：
 *  - Index Block                          —— 16-20 bytes
 *  - MetaIndex Block                      —— 16-20 bytes
 *  - Padding填充                          —— 0-8 bytes
 *  - magic number： 固定值，标识SSTable     —— 8 bytes
 */
class Footer {
 public:
  /**
   * 页脚的编码长度。48
   * 请注意，页脚的序列化将始终恰好占据这么多字节。
   * 它由两个块句柄和一个幻数(magic number)组成。
   * 
   */
  /// Encoded length of a Footer.  Note that the serialization of a
  /// Footer will always occupy exactly this many bytes.  It consists
  /// of two block handles and a magic number.
  enum { kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8 };

  Footer() = default;

  /// The block handle for the metaindex block of the table
  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  /// The block handle for the index block of the table
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

/// kTableMagicNumber was picked by running
///    echo http://code.google.com/p/leveldb/ | sha1sum
/// and taking the leading 64 bits.
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

/// 1-byte type + 32-bit crc
/// 应该用来校验的数据的字节数
static const size_t kBlockTrailerSize = 5;

struct BlockContents {
  Slice data;           /// Actual contents of data
  bool cachable;        /// True iff data can be cached
  bool heap_allocated;  /// True iff caller should delete[] data.data()
};

/// Read the block identified by "handle" from "file".  On failure
/// return non-OK.  On success fill *result and return OK.
Status ReadBlock(RandomAccessFile* file, const ReadOptions& options,
                 const BlockHandle& handle, BlockContents* result);

// Implementation details follow.  Clients should ignore,

/**
 * @brief Construct a new Block Handle:: Block Handle object
 * 
 * 将 offset_ 和 size_ 初始化为无穷大
 */
inline BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)), size_(~static_cast<uint64_t>(0)) {}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_FORMAT_H_
