// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/filename.h"

#include <cassert>
#include <cstdio>

#include "db/dbformat.h"
#include "leveldb/env.h"
#include "util/logging.h"

namespace leveldb {

/// A utility routine: write "data" to the named file and Sync() it.
Status WriteStringToFileSync(Env* env, const Slice& data,
                             const std::string& fname);

/**
 * @brief MakeFileName
 * 
 * dbname/<number>.<suffix>
 * 
 * @param dbname 
 * @param number 
 * @param suffix 
 * @return std::string 
 */
static std::string MakeFileName(const std::string& dbname, uint64_t number,
                                const char* suffix) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

/**
 * @brief LogFileName
 * 
 * dbname/<number>.log
 * 
 * @param dbname 
 * @param number 
 * @return std::string 
 */
std::string LogFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "log");
}

/**
 * @brief TableFileName
 * 
 * dbname/<number>.ldb
 * 
 * @param dbname 
 * @param number 
 * @return std::string 
 */
std::string TableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "ldb");
}

/**
 * @brief SSTTableFileName
 * 
 * dbname/<number>.sst
 * 
 * @param dbname 
 * @param number 
 * @return std::string 
 */
std::string SSTTableFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "sst");
}

/**
 * @brief 构造MANIFEST文件名
 * 
 * @param dbname 
 * @param number 
 * @return std::string 
 */
std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

/// dbname/CURRENT
std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

/// dbname/LOCK
std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

/**
 * @brief 构建临时文件名称 dbname/6位数字.dbtmp
 * 
 * @param dbname 
 * @param number 
 * @return std::string 
 */
std::string TempFileName(const std::string& dbname, uint64_t number) {
  assert(number > 0);
  return MakeFileName(dbname, number, "dbtmp");
}

/// dbname/LOG
std::string InfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG";
}

/// Return the name of the old info log file for "dbname".
/// dbname/LOG.old
std::string OldInfoLogFileName(const std::string& dbname) {
  return dbname + "/LOG.old";
}

/// Owned filenames have the form:
///    dbname/CURRENT
///    dbname/LOCK
///    dbname/LOG
///    dbname/LOG.old
///    dbname/MANIFEST-[0-9]+
///    dbname/[0-9]+.(log|sst|ldb)
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type) {
  Slice rest(filename);
  if (rest == "CURRENT") {
    *number = 0;
    *type = kCurrentFile;
  } else if (rest == "LOCK") {
    *number = 0;
    *type = kDBLockFile;
  } else if (rest == "LOG" || rest == "LOG.old") {
    *number = 0;
    *type = kInfoLogFile;
  } else if (rest.starts_with("MANIFEST-")) {
    rest.remove_prefix(strlen("MANIFEST-"));
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    if (!rest.empty()) {
      return false;
    }
    *type = kDescriptorFile;
    *number = num;
  } else {
    // Avoid strtoull() to keep filename format independent of the
    // current locale
    uint64_t num;
    if (!ConsumeDecimalNumber(&rest, &num)) {
      return false;
    }
    Slice suffix = rest;
    if (suffix == Slice(".log")) {
      *type = kLogFile;
    } else if (suffix == Slice(".sst") || suffix == Slice(".ldb")) {
      *type = kTableFile;
    } else if (suffix == Slice(".dbtmp")) {
      *type = kTempFile;
    } else {
      return false;
    }
    *number = num;
  }
  return true;
}

/**
 * @brief 创建CURRENT文件
 * 
 * 1. 创建临时文件
 * 2. 向临时文件写入当前MANIFEST文件名
 * 3. 将临时文件改名为CURRENT（mv操作？）
 * 4. 删除临时文件
 * 
 * @param env 
 * @param dbname 
 * @param descriptor_number 
 * @return Status 
 */
Status SetCurrentFile(Env* env, const std::string& dbname,
                      uint64_t descriptor_number) {
  // Remove leading "dbname/" and add newline to manifest file name
  // 删除开头的“dbname/”，并在清单文件名中添加换行符
  std::string manifest = DescriptorFileName(dbname, descriptor_number);
  Slice contents = manifest;
  assert(contents.starts_with(dbname + "/"));
  contents.remove_prefix(dbname.size() + 1);
  std::string tmp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFileSync(env, contents.ToString() + "\n", tmp);
  if (s.ok()) {
    s = env->RenameFile(tmp, CurrentFileName(dbname));
  }
  if (!s.ok()) {
    env->RemoveFile(tmp);
  }
  return s;
}

}  // namespace leveldb
