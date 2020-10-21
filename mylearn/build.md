# 如何编译和使用 leveldb

- [1. POSIX(Linux/Max)](#1-posixlinuxmax)
  - [1.1. 编译](#11-编译)
  - [1.2. 简单使用](#12-简单使用)
  - [1.3. 通过静态库使用](#13-通过静态库使用)

## 1. POSIX(Linux/Max)

> 参考：<https://zhuanlan.zhihu.com/p/67833030>

下面是在Ubuntu20.04环境下进行的实践。

### 1.1. 编译

- 依赖安装：

```shell
sudo apt-get install build-essential   #GCC
sudo apt install cmake #cmake
```

- 获取代码：

```shell
git clone --recurse-submodules https://github.com/google/leveldb.git
```

> 注意一定要--recurse-submodules选项，用于将子模块拷贝下来
> 如果忘了，可以用下列命令进行补救
> ```
> git submodule update --init --recursive
> ```

- 编译：

```shell
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
```

完成上述几步，就可以在build目录下，编译出一个静态库、一个动态库和一些测试程序。我们可以自己写一个测试代码进行测试。

### 1.2. 简单使用

> 

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
    leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
    assert(status.ok());
    int count = 0;

    //循环多次，不断添加内容
    while (count < 10) {
        std::stringstream keys ;
        std::string key;
        std::string value = "I'm fine.";

        keys << "How are you?" << count;
        key = keys.str();
        //if (s.ok()) s = db->Put(leveldb::WriteOptions(), key2, value);
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

测试程序功能很简单，就是向KV数据库添加10个KV数据。

编译运行:

```shell
g++ -o test test.cpp libleveldb.a -lpthread -I../include
./test
```

**注意：**
这里需要指明**静态库地址**和 **leveldb 头文件路径**，比较麻烦，可以将静态库安装到系统环境中。

### 1.3. 通过静态库使用

下面是安装静态库使用到的Makefile：

```Makefile
INSTALL_PATH := /usr/local
INCLUDE_DIR := #leveldb # 由于代码中的头文件都放在include/leveldb目录下，因此这里不需要另外指明为leveldb
LIBRARY_DIR := build
LIBRARY := libleveldb.a

install:install-static-lib

install-headers:
    install -d $(INSTALL_PATH)/lib
    install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)
    for header_dir in `find "include" -type d | sed '1d' | sed 's/^include\///g' `; do \
        install -d $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header_dir; \
    done
    for header in `find "include/" -type f -name "*.h" | sed 's/^include\///g' `; do \
        install -C -m 644 include/$$header $(INSTALL_PATH)/include/$(INCLUDE_DIR)/$$header; \
    done

install-static-lib: install-headers 
    install -C -m 755 $(LIBRARY_DIR)/$(LIBRARY) $(INSTALL_PATH)/lib

uninstall:
    rm -rf $(INSTALL_PATH)/include/$(INCLUDE_DIR)/leveldb \
    $(INSTALL_PATH)/lib/$(LIBRARY)
```

可以进行安装或者卸载：

```shell
make install  # 安装静态库
make uninstall # 卸载静态库
```

可能出现下列问题:

```shell
$ make
install -d /usr/local/lib
install -d /usr/local/include/leveldb
install: 无法更改 “/usr/local/include/leveldb” 的权限: 没有那个文件或目录
```

原因是当前用户没有对应文件目录的权利，运行下面命令可以解决：

```shell
sudo chown -R ${USER} /usr/local/include/
sudo chown -R ${USER} /usr/local/lib/
```

这样当前用户就拥有了在这两个目录的读写、创建子目录的权利。

也可以直接尝试下列命令：

```shell
sudo chown -R ${USER} /usr/local/
```

安装好静态库后，之前编译的命令就变成：

```shell
g++ -o test test.cpp -lleveldb -lpthread
```

注意，两个静态库指明的顺序不能颠倒，否则就会出现以下所示的错误。原因是前面一个静态库用到后面一个静态，但更具体的原理还有待考究。

```shell
$ g++ -o test test.cpp -lpthread -lleveldb
/usr/bin/ld: /usr/local/lib/libleveldb.a(env_posix.cc.o): in function `leveldb::(anonymous namespace)::PosixEnv::StartThread(void (*)(void*), void*)':
env_posix.cc:(.text+0x332): undefined reference to `pthread_create'
/usr/bin/ld: /usr/local/lib/libleveldb.a(env_posix.cc.o): in function `leveldb::(anonymous namespace)::PosixEnv::Schedule(void (*)(void*), void*)':
env_posix.cc:(.text+0xcaa): undefined reference to `pthread_create'
collect2: error: ld returned 1 exit status
```
