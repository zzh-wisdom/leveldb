// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// The representation of a DBImpl consists of a set of Versions.  The
// newest version is called "current".  Older versions may be kept
// around to provide a consistent view to live iterators.
//
// Each Version keeps track of a set of Table files per level.  The
// entire set of versions is maintained in a VersionSet.
//
// Version,VersionSet are thread-compatible, but require external
// synchronization on all accesses.

#ifndef STORAGE_LEVELDB_DB_VERSION_SET_H_
#define STORAGE_LEVELDB_DB_VERSION_SET_H_

#include <map>
#include <set>
#include <vector>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "port/port.h"
#include "port/thread_annotations.h"

namespace leveldb {

namespace log {
class Writer;
}

class Compaction;
class Iterator;
class MemTable;
class TableBuilder;
class TableCache;
class Version;
class VersionSet;
class WritableFile;

// Return the smallest index i such that files[i]->largest >= key.
// Return files.size() if there is no such file.
// REQUIRES: "files" contains a sorted list of non-overlapping files.
int FindFile(const InternalKeyComparator& icmp,
             const std::vector<FileMetaData*>& files, const Slice& key);

// Returns true iff some file in "files" overlaps the user key range
// [*smallest,*largest].
// smallest==nullptr represents a key smaller than all keys in the DB.
// largest==nullptr represents a key largest than all keys in the DB.
// REQUIRES: If disjoint_sorted_files, files[] contains disjoint ranges
//           in sorted order.
bool SomeFileOverlapsRange(const InternalKeyComparator& icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key);

/**
 * @brief Version
 * 
 * Version通过Version* prev和*next指针构成了一个Version双向循环链表.
 * 表头指针则在VersionSet中（初始都指向自己）。
 * 
 * 所有Version公用vset_中的table cache
 * 
 * 当Get操作直接搜寻memtable没有命中时，就需要调用Version::Get()函数从磁盘load数据文件并查找。
 * 如果此次Get不止seek了一个文件，就记录第一个文件到stat并返回。其后leveldb就会调用UpdateStats(stat)。
 * 
 * Stat表明在指定key range查找key时，都要先seek此文件，才能在后续的sstable文件中找到key。
 * 函数是将stat记录的sstable文件的allowed_seeks减1，减到0就执行compaction。
 * 也就是说如果文件被seek的次数超过了限制，表明读取效率已经很低，需要执行compaction了。
 * 所以说allowed_seeks是对compaction流程的有一个优化。
 * 
 */
class Version {
 public:
  struct GetStats {
    FileMetaData* seek_file;
    int seek_file_level;
  };

  /// Append to *iters a sequence of iterators that will
  /// yield the contents of this Version when merged together.
  /// @pre REQUIRES: This version has been saved (see VersionSet::SaveTo)
  void AddIterators(const ReadOptions&, std::vector<Iterator*>* iters);

  /// Lookup the value for key.  If found, store it in *val and
  /// return OK.  Else return a non-OK status.  Fills *stats.
  /// @pre REQUIRES: lock is not held
  Status Get(const ReadOptions&, const LookupKey& key, std::string* val,
             GetStats* stats);

  /// Adds "stats" into the current state.  Returns true if a new
  /// compaction may need to be triggered, false otherwise.
  /// 该函数会更新file_to_compact_，file_to_compact_level_。
  /// @pre REQUIRES: lock is held
  bool UpdateStats(const GetStats& stats);

  /// Record a sample of bytes read at the specified internal key.
  /// Samples are taken approximately once every config::kReadBytesPeriod
  /// bytes.  Returns true if a new compaction may need to be triggered.
  /// 该抽样是为了提高compaction的频率，提前发现需要compaction的l0 sstable
  /// @pre REQUIRES: lock is held
  bool RecordReadSample(Slice key);

  /// Reference count management (so Versions do not disappear out from
  /// under live iterators)
  void Ref();
  /// 计数等于0则释放this类
  void Unref();

  /// 在指定level中找出和[begin, end]有重合的sstable文件
  /// 这个重叠与常规的理解不一样，查找时如果一个文件与[begin, end]有重叠，则要根据的file的(min和max)key范围扩展范围，
  void GetOverlappingInputs(
      int level,
      const InternalKey* begin,  // nullptr means before all keys
      const InternalKey* end,    // nullptr means after all keys
      std::vector<FileMetaData*>* inputs);

  /// Returns true iff some file in the specified level overlaps(重叠)
  /// some part of [*smallest_user_key,*largest_user_key].
  /// smallest_user_key==nullptr represents a key smaller than all the DB's keys.
  /// largest_user_key==nullptr represents a key largest than all the DB's keys.
  /// 检查指定level文件是否与范围[*smallest_user_key,*largest_user_key]有重合
  bool OverlapInLevel(int level, const Slice* smallest_user_key,
                      const Slice* largest_user_key);

  /// Return the level at which we should place a new memtable compaction
  /// result that covers the range [smallest_user_key,largest_user_key].
  /// 函数返回我们应该在哪个level上放置新的memtable compaction，
  /// 这个compaction覆盖了范围[smallest_user_key,largest_user_key]。
  int PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                 const Slice& largest_user_key);
  /// 指定level的sstable个数
  int NumFiles(int level) const { return files_[level].size(); }

  /// Return a human readable string that describes this version's contents.
  std::string DebugString() const;

 private:
  friend class Compaction;
  friend class VersionSet;

  class LevelFileNumIterator;

  explicit Version(VersionSet* vset)
      : vset_(vset),
        next_(this),
        prev_(this),
        refs_(0),
        file_to_compact_(nullptr),
        file_to_compact_level_(-1),
        compaction_score_(-1),
        compaction_level_(-1) {}

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  ~Version();

  Iterator* NewConcatenatingIterator(const ReadOptions&, int level) const;

  // Call func(arg, level, f) for every file that overlaps user_key in
  // order from newest to oldest.  If an invocation of func returns
  // false, makes no more calls.
  //
  // REQUIRES: user portion of internal_key == user_key.
  void ForEachOverlapping(Slice user_key, Slice internal_key, void* arg,
                          bool (*func)(void*, int, FileMetaData*));

  VersionSet* vset_;  /// VersionSet to which this Version belongs
  Version* next_;     /// Next version in linked list
  Version* prev_;     /// Previous version in linked list
  int refs_;          /// Number of live refs to this version

  /// List of files per level，sstable文件列表，存活的临时文件也会加入到这里
  std::vector<FileMetaData*> files_[config::kNumLevels];

  /// Next file to compact based on seek stats. 下一个要compact的文件
  FileMetaData* file_to_compact_;
  int file_to_compact_level_;

  /// Level that should be compacted next and its compaction score.
  /// Score < 1 means compaction is not strictly needed.  These fields
  /// are initialized by Finalize().
  double compaction_score_;  /// 分数大于1则需要进行压缩
  int compaction_level_;   /// 必定小于最高的level
};

/**
 * @brief VersionSet
 * 
 * VersionSet是所有Version的集合，这是个version的管理机构。
 * 
 * VersionSet会使用到TableCache，这个是调用者传入的。TableCache用于Get k/v操作。
 * 
 */
class VersionSet {
 public:
  VersionSet(const std::string& dbname, const Options* options,
             TableCache* table_cache, const InternalKeyComparator*);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  ~VersionSet();

  /// Apply *edit to the current version to form a new descriptor that
  /// is both saved to persistent state and installed as the new
  /// current version.  Will release *mu while actually writing to the file.
  /// 在current version上应用指定的VersionEdit，生成新的MANIFEST信息，保存到磁盘上，并用作current version。
  /// 写文件的期间会解锁🔐
  /// REQUIRES: *mu is held on entry.
  /// REQUIRES: no other thread concurrently calls LogAndApply()
  Status LogAndApply(VersionEdit* edit, port::Mutex* mu)
      EXCLUSIVE_LOCKS_REQUIRED(mu);

  /// Recover the last saved descriptor from persistent storage.
  /// 恢复数据，从磁盘恢复最后保存的元信息
  Status Recover(bool* save_manifest);

  /// Return the current version.
  Version* current() const { return current_; }

  /// Return the current manifest file number
  /// 返回当前元信息的文件（manifest）编号
  uint64_t ManifestFileNumber() const { return manifest_file_number_; }

  /// Allocate and return a new file number
  /// next_file_number_++
  uint64_t NewFileNumber() { return next_file_number_++; }

  /// Arrange to reuse "file_number" unless a newer file number has
  /// already been allocated.
  /// 除非已经分配了新的文件编号，否则请重新使用“ file_number”。
  /// @file_number必须是最后分配的那个
  /// 重用是通过将next_file_number_回退1来实现的
  /// REQUIRES: "file_number" was returned by a call to NewFileNumber().
  void ReuseFileNumber(uint64_t file_number) {
    if (next_file_number_ == file_number + 1) {
      next_file_number_ = file_number;
    }
  }

  /// Return the number of Table files at the specified level.
  /// 返回指定level的table文件个数
  int NumLevelFiles(int level) const;

  /// Return the combined file size of all files at the specified level.
  /// 返回指定level中所有sstable文件大小的和
  int64_t NumLevelBytes(int level) const;

  /// Return the last sequence number.
  uint64_t LastSequence() const { return last_sequence_; }

  /// Set the last sequence number to s.
  /// 不能回退
  void SetLastSequence(uint64_t s) {
    assert(s >= last_sequence_);
    last_sequence_ = s;
  }

  /// Mark the specified file number as used.
  /// 标记指定的文件编号已经被使用了
  /// 通过设置 next_file_number_ = number + 1 来实现
  void MarkFileNumberUsed(uint64_t number);

  /// Return the current log file number.
  /// 返回当前log文件编号
  uint64_t LogNumber() const { return log_number_; }

  /// Return the log file number for the log file that is currently
  /// being compacted, or zero if there is no such log file.
  /// 返回正在compact（即上一个写满）的log文件编号，如果没有返回0
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  // Pick level and inputs for a new compaction.
  // Returns nullptr if there is no compaction to be done.
  // Otherwise returns a pointer to a heap-allocated object that
  // describes the compaction.  Caller should delete the result.
  Compaction* PickCompaction();

  // Return a compaction object for compacting the range [begin,end] in
  // the specified level.  Returns nullptr if there is nothing in that
  // level that overlaps the specified range.  Caller should delete
  // the result.
  Compaction* CompactRange(int level, const InternalKey* begin,
                           const InternalKey* end);

  /// Return the maximum overlapping data (in bytes) at next level for any
  /// file at a level >= 1.
  /// 对于所有level>0，遍历文件，找到单个文件和下一层所有文件的重叠数据的最大值(in bytes)
  /// 这个就是Version:: GetOverlappingInputs()函数的简单应用
  int64_t MaxNextLevelOverlappingBytes();

  // Create an iterator that reads over the compaction inputs for "*c".
  // The caller should delete the iterator when no longer needed.
  Iterator* MakeInputIterator(Compaction* c);

  // Returns true iff some level needs a compaction.
  bool NeedsCompaction() const {
    Version* v = current_;
    return (v->compaction_score_ >= 1) || (v->file_to_compact_ != nullptr);
  }

  /// Add all files listed in any live version to *live.
  /// May also mutate some internal state.
  /// 获取函数，把所有version的所有level的文件编号加入到@live中
  /// 也可能会改变某些内部状态。
  void AddLiveFiles(std::set<uint64_t>* live);

  // Return the approximate offset in the database of the data for
  // "key" as of version "v".
  uint64_t ApproximateOffsetOf(Version* v, const InternalKey& key);

  /// Return a human-readable short (single-line) summary of the number
  /// of files per level.  Uses *scratch as backing store.
  struct LevelSummaryStorage {
    char buffer[100];
  };
  /// 返回一个可读的单行信息——每个level的文件数，保存在*scratch中
  const char* LevelSummary(LevelSummaryStorage* scratch) const;

 private:
  class Builder;

  friend class Compaction;
  friend class Version;

  bool ReuseManifest(const std::string& dscname, const std::string& dscbase);

  void Finalize(Version* v);

  void GetRange(const std::vector<FileMetaData*>& inputs, InternalKey* smallest,
                InternalKey* largest);

  void GetRange2(const std::vector<FileMetaData*>& inputs1,
                 const std::vector<FileMetaData*>& inputs2,
                 InternalKey* smallest, InternalKey* largest);

  void SetupOtherInputs(Compaction* c);

  // Save current contents to *log
  Status WriteSnapshot(log::Writer* log);

  void AppendVersion(Version* v);

  // 第一组，直接来自于DBImple，构造函数传入
  Env* const env_;    /// 操作系统封装，平台相关的环境
  const std::string dbname_;
  const Options* const options_;
  TableCache* const table_cache_; /// 指向sst文件的cache对象；
  const InternalKeyComparator icmp_;  /// InternalKey比较器；
  // 第二组，db元信息相关
  uint64_t next_file_number_;  /// 下一个文件编号，文件编号计数器；初始为2，所有文件类型共享一份文件编号计数器
  uint64_t manifest_file_number_;   /// manifest文件编号，初始为0
  uint64_t last_sequence_; /// 上一个序列号（快照就是靠它实现的），初始为0
  uint64_t log_number_;  /// log 文件编号，初始为0
  uint64_t prev_log_number_;  /// 0 or backing store for memtable being compacted，初始为0

  // 第三组，menifest文件相关
  /// Opened lazily
  WritableFile* descriptor_file_; /// 指向可写的manifest文件（log文件格式）；
  log::Writer* descriptor_log_;   /// 指向log writer对象，用于向descriptor_file_写数据
  // 第四组，版本管理
  Version dummy_versions_;  /// Head of circular doubly-linked list of versions. 双向链表的head，初始为以this为参数构造的Version
  Version* current_;        /// == dummy_versions_.prev_  当前版本

  /// Per-level key at which the next compaction at that level should start.
  /// Either an empty string, or a valid InternalKey.
  /// level下一次compaction的开始key，空字符串或者合法的InternalKey
  std::string compact_pointer_[config::kNumLevels];
};

// A Compaction encapsulates information about a compaction.
/**
 * @brief 
 * 
 * Compaction封装有关压缩的信息。
 * 
 * compaction将会合并level和level+1中的部分文件
 * 
 */
class Compaction {
 public:
  ~Compaction();

  // Return the level that is being compacted.  Inputs from "level"
  // and "level+1" will be merged to produce a set of "level+1" files.
  int level() const { return level_; }

  // Return the object that holds the edits to the descriptor done
  // by this compaction.
  VersionEdit* edit() { return &edit_; }

  /// "which" must be either 0 or 1
  int num_input_files(int which) const { return inputs_[which].size(); }

  // Return the ith input file at "level()+which" ("which" must be 0 or 1).
  FileMetaData* input(int which, int i) const { return inputs_[which][i]; }

  // Maximum size of files to build during this compaction.
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // Is this a trivial compaction that can be implemented by just
  // moving a single input file to the next level (no merging or splitting)
  bool IsTrivialMove() const;

  // Add all inputs to this compaction as delete operations to *edit.
  void AddInputDeletions(VersionEdit* edit);

  /// Returns true if the information we have available guarantees that
  /// the compaction is producing data in "level+1" for which no data exists
  /// in levels greater than "level+1".
  /// 压缩在level + 1生成的数据（key）没有在更高层中存在（即没有文件范围包含这个key），则返回true
  bool IsBaseLevelForKey(const Slice& user_key);

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  bool ShouldStopBefore(const Slice& internal_key);

  // Release the input version for the compaction, once the compaction
  // is successful.
  void ReleaseInputs();

 private:
  friend class Version;
  friend class VersionSet;

  Compaction(const Options* options, int level);

  int level_;     /// 当前压缩的level，构造时根据传入的参数初始化
  uint64_t max_output_file_size_;  /// 压缩后输出文件的最大大小，根据用户的option初始化
  Version* input_version_;  /// 压缩时的当前版本
  VersionEdit edit_;   /// 带回压缩后的元数据变化信息

  /// Each compaction reads inputs from "level_" and "level_+1"
  std::vector<FileMetaData*> inputs_[2];  /// The two sets of inputs

  /// State used to check for number of overlapping grandparent files
  /// (parent == level_ + 1, grandparent == level_ + 2)
  /// 保存祖母level（即level + 2）与当前压缩的所有文件组成的范围重叠的文件
  /// 通过grandparents_数组，我们可以控制新合并生成放在level+1中的每个sstable文件不要和level+2中的过多文件有key范围重合
  std::vector<FileMetaData*> grandparents_;
  size_t grandparent_index_;  /// Index in grandparent_starts_，grandparents_数据的下表索引
  /// 保证每个合并而成的sstable中都有key-value数据。想象一个场景，
  /// 如果合并过程中第一个打算加入的key是一个比level+2中很多文件的最大key都大的数值，
  /// 则我们可能会误以为当前合并而成的文件已经足够大了，准备把它写盘，但是实际却是当前构建文件中还没有key-value，于是就会出现问题。
  bool seen_key_;             /// Some output key has been seen
  int64_t overlapped_bytes_;  /// Bytes of overlap between current output
                              /// and grandparent files，用以防止生成的单个文件与level+2的太多文件重叠

  // State for implementing IsBaseLevelForKey

  // level_ptrs_ holds indices into input_version_->levels_: our state
  // is that we are positioned at one of the file ranges for each
  // higher level than the ones involved in this compaction (i.e. for
  // all L >= level_ + 2).
  /// 存储的是整型变量，记录每次遍历各层文件时的下标信息，主要用于判断一个key是否在level+2以及更高层的的文件中。
  /// 可以看IsBaseLevelForKey函数的实现。level_ptrs_[]记录是某个key的上界文件下标
  size_t level_ptrs_[config::kNumLevels];
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_VERSION_SET_H_
