# 代码总揽

## Coding

varint：<https://developers.google.com/protocol-buffers/docs/encoding#varints>

Varint32、Varint64: 变长
Fixed32、Fixed64: 定长

一个Fixed32编码成Varint32，最多只用5个字节。（7*5=35 > 32）

注意的是，代码区分Varint32和Varint64。
GetVarint32Ptr 里面多加了一个对只有一个字节的数的判断
GetVarint64Ptr
这里不用同一个逻辑，估计是因为Varint32类型的数一般比较小。

## Key

### InternalKey

由多部分组成的字符串。

- 内容：｜User key (string) | sequence number (7 bytes) | value type (1 byte) |

- 结构：key(string) + tag(Fixed64)

sequence number是所有基于op log系统的关键数据，它唯一指定了不同操作的时间顺序。

把user key放到前面的原因是，这样对同一个user key的操作就可以按照sequence number顺序连续存放了，不同的user key是互不相干的。

### ParsedInternalKey

对InternalKey分拆后的结果。

结构体成员如下：

```cpp
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;
};
```

### LookupKey & Memtable Key

Memtable的查询接口传入的是LookupKey，它由User Key和Sequence Number组合而成。

- 结构：| Size (int32变长)| User key (string) | sequence number (7 bytes) | value type (1 byte) |
  - Size是user key长度+8，也就是整个字符串长度了；
  - value type是kValueTypeForSeek，它等于kTypeValue。
  - 由于LookupKey的size是变长存储的，因此它使用kstart_记录了user key string的起始地址，否则将不能正确的获取size和user key；

LookupKey中的字符串固定分配大小，防止小空间的频繁申请。

## Comparator

抽象类--接口

BytewiseComparatorImpl：内置字典顺序比较器

InternalKeyComparator：Internalkey比较器，User key部分使用用户传入的比较器，seq则按照降序排序

## Memtable

<https://blog.csdn.net/sparkliang/article/details/8604416>

Memtable

Immutable Memtable

Memtable提供了写入KV记录，删除以及读取KV记录的接口，但是事实上Memtable并不执行真正的删除操作,删除某个Key的Value在Memtable内是作为插入一条记录实施的，但是会打上一个Key的删除标记，真正的删除操作在后面的 Compaction过程中，lazy delete。

基于Skip list实现。

Memtable只是一个结构类。



entry组成：

- klength  **varint32**      包括userkey和tag的长度，即ukey_size + 8
- internal_key char[klength]
  - userkey  char[ukey_size]
  - tag      uint64   高7个字节是 SequenceNumber，最低一个字节是 ValueType
- vlength  **varint32**
- value    char[vlength]

## Log

所有的写操作都必须先成功的append到操作日志中，然后再更新内存memtable。这样做有两个有点：1可以将随机的写IO变成append，极大的提高写磁盘速度；2防止在节点down机导致内存数据丢失，造成数据丢失，这对系统来说是个灾难。

见：./doc/log_format.md

相关类：

- log::Writer
- log::Reader

## Bloom Filter

使用double hashing来模拟多个hash函数。

## 其他知识

内存映射文件的读写效率：https://blog.csdn.net/mg0832058/article/details/5890688























