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
    cout << status.ToString() << endl;
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