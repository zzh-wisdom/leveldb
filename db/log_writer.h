// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_WRITER_H_
#define STORAGE_LEVELDB_DB_LOG_WRITER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class WritableFile;

namespace log {

/**
 * @brief log::Writer
 * 
 * 只有一个接口：AddRecord
 * 
 */
class Writer {
 public:
  /// Create a writer that will append data to "*dest".
  /// @attention
  /// "*dest" must be initially empty.
  /// "*dest" must remain live while this Writer is in use.
  explicit Writer(WritableFile* dest);

  /// Create a writer that will append data to "*dest".
  /// @attention
  /// "*dest" must have initial length "dest_length".
  /// "*dest" must remain live while this Writer is in use.
  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  /// Current offset in block

  /// crc32c values for all supported record types.  These are
  /// pre-computed to reduce the overhead of computing the crc of the
  /// record type stored in the header.
  uint32_t type_crc_[kMaxRecordType + 1];  /// 存放的为Record Type预先计算的CRC32值
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_WRITER_H_
