// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VERSION_EDIT_H_
#define STORAGE_LEVELDB_DB_VERSION_EDIT_H_

#include <set>
#include <utility>
#include <vector>

#include "db/dbformat.h"

namespace leveldb {

class VersionSet;

struct FileMetaData {
  FileMetaData() : refs(0), allowed_seeks(1 << 30), file_size(0) {}

  int refs;
  int allowed_seeks;     /// Seeks allowed until compaction，指示该文件可以被查找不成功的次数，减为0则意味着利用率非常低，需要compaction
  uint64_t number;       /// 文件编号
  uint64_t file_size;    /// File size in bytes
  InternalKey smallest;  /// Smallest internal key served by table（包含）
  InternalKey largest;   /// Largest internal key served by table（包含）
};

/**
 * @brief VersionEdit
 * 
 * compact过程中会有一系列改变当前Version的操作（FileNumber增加，删除input的sstable,增加输出的sstable），
 * 为了缩小version切换的时间点，将这些操作封装成versionedit，
 * compact完成时，将versionedit中的操作一次应用到当前version即可得到最新状态的version。
 * 
 * VersionEdit记录了Version之间的变化，相当于delta增量，
 * 表示又增加了多少文件，删除了文件。也就是说：Version0 + VersionEdit --> Version1。
 * 
 * LevelDB中对Manifest的Decode/Encode是通过类VersionEdit完成的，Menifest文件保存了LevelDB的管理元信息。
 * 
 * VersionEdit并不操作文件，只是为Manifest文件读写准备好数据、从读取的数据中解析出DB元信息。
 * 
 * VersionEdit有两个作用：
 * 1. 当版本间有增量变动时，VersionEdit记录了这种变动；
 * 2. 写入到MANIFEST时，先将current version的db元信息保存到一个VersionEdit中，然后在组织成一个log record写入文件；
 * 
 */
class VersionEdit {
 public:
  VersionEdit() { Clear(); }
  ~VersionEdit() = default;

  /**
   * @brief 清空信息
   * 
   * 除了 compact_pointers_，其他均清空
   * 
   */
  void Clear();

  // 设置元信息的函数
  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFile(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(SequenceNumber seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }
  void SetCompactPointer(int level, const InternalKey& key) {
    compact_pointers_.push_back(std::make_pair(level, key));
  }

  // Add the specified file at the specified number.
  // REQUIRES: This version has not been saved (see VersionSet::SaveTo)
  // REQUIRES: "smallest" and "largest" are smallest and largest keys in file
  /**
   * @brief Add the specified file at the specified number.
   * 
   * 根据参数生产一个FileMetaData对象，把sstable文件信息（level->FileMetaData pair）添加到new_files_数组中。
   * 
   * REQUIRES: This version has not been saved (see VersionSet::SaveTo)
   * REQUIRES: "smallest" and "largest" are smallest and largest keys in file
   * 
   * @param level 
   * @param file 文件编号
   * @param file_size 
   * @param smallest smallest keys in file
   * @param largest largest keys in file
   */
  void AddFile(int level, uint64_t file, uint64_t file_size,
               const InternalKey& smallest, const InternalKey& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest;
    f.largest = largest;
    new_files_.push_back(std::make_pair(level, f));
  }

  /**
   * @brief Delete the specified "file" from the specified "level".
   * 
   * 插入到deleted_files_集合中
   * 
   * @param level 
   * @param file 文件编号
   */
  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert(std::make_pair(level, file));
  }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  std::string DebugString() const;

 private:
  friend class VersionSet;

  typedef std::set<std::pair<int, uint64_t>> DeletedFileSet;  

  std::string comparator_;  /// key comparator名字
  uint64_t log_number_;     /// log 文件编号
  uint64_t prev_log_number_; /// 前一个日志文件编号
  uint64_t next_file_number_; /// 下一个文件编号
  SequenceNumber last_sequence_; /// 上一个seq(版本)
  bool has_comparator_;  /// 是否设置了comparator名字
  bool has_log_number_;   /// 是否设置了log文件编号
  bool has_prev_log_number_; /// 是否设置了前一个日志文件编号
  bool has_next_file_number_; /// 是否设置了下一个文件编号
  bool has_last_sequence_; /// 是否设置了上一个seq(版本)

  std::vector<std::pair<int, InternalKey>> compact_pointers_; /// compaction的位置，level->internalKey pair
  DeletedFileSet deleted_files_;  /// 删除文件集合，level->文件编号的pair
  std::vector<std::pair<int, FileMetaData>> new_files_;  /// 新文件集合，level->FileMetaData pair
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_EDIT_H_
