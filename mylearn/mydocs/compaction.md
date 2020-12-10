# Compaction



两种触发条件：

- file_to_compact_ != nullptr 某个文件seek足够的次数（不命中），需要compact
- compaction_score_ >= 1 某一层的大小达到阈值

