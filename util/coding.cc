// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

namespace leveldb {

/**
 * @brief PutFixed32
 * 
 * 在dst末尾附加value按照小端编码后的固定4个字节
 * 
 * @param dst 
 * @param value 
 */
void PutFixed32(std::string* dst, uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

/**
 * @brief PutFixed64
 * 
 * 在dst末尾附加value按照小端编码后的固定8个字节
 * 
 * @param dst 
 * @param value 
 */
void PutFixed64(std::string* dst, uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

/**
 * @brief EncodeVarint32
 * 
 * @param dst 
 * @param v 
 * @return char* 编码的Varint32数据的下一地址
 */
char* EncodeVarint32(char* dst, uint32_t v) {
  // Operate on characters as unsigneds
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  static const int B = 128;
  if (v < (1 << 7)) {
    *(ptr++) = v;
  } else if (v < (1 << 14)) {
    *(ptr++) = v | B;  // 截断复制
    *(ptr++) = v >> 7;
  } else if (v < (1 << 21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = v >> 14;
  } else if (v < (1 << 28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = v >> 21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v >> 7) | B;
    *(ptr++) = (v >> 14) | B;
    *(ptr++) = (v >> 21) | B;
    *(ptr++) = v >> 28;
  }
  return reinterpret_cast<char*>(ptr);
}

/**
 * @brief PutVarint32
 * 
 * 将uint32_t编码成Varint32，附加到dst尾部
 * 
 * @param dst 
 * @param v 
 */
void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  char* ptr = EncodeVarint32(buf, v);
  dst->append(buf, ptr - buf);
}

/**
 * @brief EncodeVarint64
 * 
 * @param dst 
 * @param v 
 * @return char* 
 */
char* EncodeVarint64(char* dst, uint64_t v) {
  static const int B = 128;
  uint8_t* ptr = reinterpret_cast<uint8_t*>(dst);
  while (v >= B) {
    *(ptr++) = v | B;
    v >>= 7;
  }
  *(ptr++) = static_cast<uint8_t>(v);
  return reinterpret_cast<char*>(ptr);
}

/**
 * @brief PutVarint64
 * 
 * @param dst 
 * @param v 
 */
void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  char* ptr = EncodeVarint64(buf, v);
  dst->append(buf, ptr - buf);
}

/**
 * @brief 先添加长度，再添加Slice数据
 * 
 * 长度：Varint32
 * 
 * @param dst 
 * @param value 
 */
void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
  PutVarint32(dst, value.size());
  dst->append(value.data(), value.size());
}

/**
 * @brief 计算v通过Varint64编码需要的字节数
 * 
 * @param v 
 * @return int 
 */
int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    len++;
  }
  return len;
}

/**
 * @brief GetVarint32PtrFallback
 * 
 * @param p 
 * @param limit 
 * @param value 
 * @return const char*  跳过解码的数据，如果解码超过limit或者不合法，返回nullptr
 */
const char* GetVarint32PtrFallback(const char* p, const char* limit,
                                   uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

/**
 * @brief GetVarint32
 * 
 * @param input 
 * @param value 
 * @return true 
 * @return false 
 */
bool GetVarint32(Slice* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

/**
 * @brief GetVarint64Ptr
 * 
 * Ptr的意思是指，原始数据是通过char*指针传入的
 * 
 * @param p 
 * @param limit 
 * @param value 
 * @return const char* 
 */
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = *(reinterpret_cast<const uint8_t*>(p));
    p++;
    if (byte & 128) {
      // More bytes are present
      result |= ((byte & 127) << shift);
    } else {
      result |= (byte << shift);
      *value = result;
      return reinterpret_cast<const char*>(p);
    }
  }
  return nullptr;
}

/**
 * @brief 从input解码Varint64数据到value
 * 
 * 注意：解码后，会将input的data_移动（跳过当前Varint64）
 * 
 * @param input 
 * @param value 
 * @return true 
 * @return false 
 */
bool GetVarint64(Slice* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  } else {
    *input = Slice(q, limit - q);
    return true;
  }
}

/**
 * @brief GetLengthPrefixedSlice
 * 
 * @param p 
 * @param limit 
 * @param result 
 * @return const char* 
 */
const char* GetLengthPrefixedSlice(const char* p, const char* limit,
                                   Slice* result) {
  uint32_t len;
  p = GetVarint32Ptr(p, limit, &len);
  if (p == nullptr) return nullptr;
  if (p + len > limit) return nullptr;
  *result = Slice(p, len);
  return p + len;
}

/**
 * @brief 获取有长度前缀的Slice
 * 
 * 先获取长度
 * 得到Slice
 * 从input删除解码出来的数据
 * 
 * @param input 
 * @param result 
 * @return true 
 * @return false 
 */
bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
  uint32_t len;
  if (GetVarint32(input, &len) && input->size() >= len) {
    *result = Slice(input->data(), len);
    input->remove_prefix(len);
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb
