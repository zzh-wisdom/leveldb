// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_READER_H_
#define STORAGE_LEVELDB_DB_LOG_READER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class SequentialFile;

namespace log {

/**
 * @brief log::Reader
 * 
 */
class Reader {
 public:
  /// Interface for reporting errors.
  class Reporter {
   public:
    virtual ~Reporter();

    /// Some corruption was detected.  "size" is the approximate number
    /// of bytes dropped due to the corruption.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  /// Create a reader that will return log records from "*file".
  /// "*file" must remain live while this Reader is in use.
  ///
  /// If "reporter" is non-null, it is notified whenever some data is
  /// dropped due to a detected corruption.  "*reporter" must remain
  /// live while this Reader is in use.
  ///
  /// If "checksum" is true, verify checksums if available.
  ///
  /// The Reader will start reading at the first record located at physical
  /// position >= initial_offset within the file.
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader();

  /// Read the next record into *record.  Returns true if read
  /// successfully, false if we hit end of the input.  May use
  /// "*scratch" as temporary storage.  The contents filled in *record
  /// will only be valid until the next mutating operation on this
  /// reader or the next mutation to *scratch.
  /**
   * 将下一条记录读入* record。如果读取成功，则返回true；
   * 如果到达输入结尾，则返回false。可以使用“ * scratch”作为临时存储。 
   * *record中填充的内容仅在该读取器上的下一个变异操作或* scratch的下一个变异之前有效。
   * 
   * Read接口有一个*result参数传递结果就行了，为何还有一个*scratch呢，这个就和Slice相关了。
   * 它的字符串指针是传入的外部char*指针，自己并不负责内存的管理与分配。
   * 因此Read接口需要调用者提供一个字符串指针，实际存放字符串的地方。
   */
  bool ReadRecord(Slice* record, std::string* scratch);

  /// Returns the physical offset of the last record returned by ReadRecord.
  /// 返回上一次调用ReadRecord返回的记录的物理偏移
  ///
  /// Undefined before the first call to ReadRecord.
  uint64_t LastRecordOffset();

 private:
  // Extend record types with the following special values
  enum {
    kEof = kMaxRecordType + 1,
    /// Returned whenever we find an invalid physical record.
    /// Currently there are three situations in which this happens:
    /// * The record has an invalid CRC (ReadPhysicalRecord reports a drop)
    /// * The record is a 0-length record (No drop is reported)
    /// * The record is below constructor's initial_offset (No drop is reported)
    kBadRecord = kMaxRecordType + 2
  };

  /// Skips all blocks that are completely before "initial_offset_".
  ///
  /// Returns true on success. Handles reporting.
  bool SkipToInitialBlock();

  /// Return type, or one of the preceding special values（前面扩展定义的枚举类型）
  unsigned int ReadPhysicalRecord(Slice* result);

  /// Reports dropped bytes to the reporter.
  /// buffer_ must be updated to remove the dropped bytes prior to invocation.
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  bool const checksum_;
  char* const backing_store_;  /// 初始化申请一个block的空间
  Slice buffer_;  /// 缓存读取的块内容，内部指针指向backing_store_
  bool eof_;  /// Last Read() indicated EOF by returning < kBlockSize 暗示到文件尾部

  /// Offset of the last record returned by ReadRecord.
  /// 函数ReadRecord返回的上一个record的偏移
  uint64_t last_record_offset_;
  /// Offset of the first location past the end of buffer_.
  /// buffer_数据末尾的偏移，即当前读取的块的下一个块的偏移。对应着物理文件当前读取到的偏移位置
  uint64_t end_of_buffer_offset_;

  /// Offset at which to start looking for the first record to return
  uint64_t const initial_offset_;

  /// True if we are resynchronizing after a seek (initial_offset_ > 0). In
  /// particular, a run of kMiddleType and kLastType records can be silently
  /// skipped in this mode
  /// 确保seek到FIRST或者FULL分片？
  bool resyncing_;   /// initial_offset > 0 ? 1 : 0
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_READER_H_
