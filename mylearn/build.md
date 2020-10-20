

## Ubuntu 20.04

> 参考：<https://zhuanlan.zhihu.com/p/67833030>

### 编译

获取代码：
git clone --recurse-submodules https://github.com/google/leveldb.git
注意一定要--recurse-submodules选项，用于将子模块拷贝下来
如果忘了，可以用下列命令进行补救
git submodule update --init --recursive

mkdir -p build && cd build

依赖安装：
sudo apt-get install build-essential   #GCC
sudo apt install cmake #cmake

编译：
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .

完成上述几步，就可以在build目录下，编译出一个静态库、一个动态库和一些测试程序。我们可以自己写一个测试代码进行测试。

### 使用

创建一个test目录， 然后将静态库libleveldb.a拷贝进来。

```shell
mkdir test
cp build/libleveldb.a test
cd test
```

然后在其中创建一个名为test.cpp的文件，文件内容如下：

```cpp
#include <assert.h>
#include <iostream>
#include <sstream>
#include "leveldb/db.h"

using namespace std;

int main(){
    leveldb::DB* db; 
    leveldb::Options options;
    options.create_if_missing = true;
    //打开一个数据库
    leveldb::Status status = leveldb::DB::Open(options,"./testdb",&db);
    assert(status.ok());
    int count = 0;

    //循环多次，不断添加内容
    while (count < 1000) {
        std::stringstream keys ;
        std::string key;
        std::string value = "shuningzhang@itworld123.com";

        keys << "itworld123-" << count;
        key = keys.str();
        status = db->Put(leveldb::WriteOptions(), key, value);//添加内容
        assert(status.ok());

        status = db->Get(leveldb::ReadOptions(), key, &value);//获取
        assert(status.ok());
        std::cout<<value<<std::endl;

        count ++; 
    }   

    delete db; //关闭数据库

    return 0;  
}
```

测试程序功能很简单，就是向KV数据库添加1000个KV数据。


编译运行:

```shell
g++ -o test test.cpp libleveldb.a -lpthread -I../include

```






