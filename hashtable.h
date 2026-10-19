#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <string>
#include <vector>
#include <functional>
#include <shared_mutex>
#include <atomic>
#include <mutex> 
#include "engine.h"
#include "mempool.h"

// 链表节点
struct HashTableNode {
    std::string key;
    std::string value;
    HashTableNode* next = nullptr;
};

class HashTable : public Engine {
public:
    HashTable();
    ~HashTable();
    bool set(const std::string& key, const std::string& value) override;
    bool get(const std::string& key, std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    int size() const override;
    void scan(std::function<bool(const std::string&, const std::string&)>) override;

private:
    static const int BUCKET_COUNT = 262144;  // 桶的数量，2 的幂次方便取模
    std::vector<HashTableNode*> buckets_;   // 桶数组，每个桶是一个链表的头指针

    // 分片锁相关
    static const int SHARD_COUNT = 8;
    struct Shard {
        std::shared_mutex rwlock;
        // ===== 方案B：每个分片独立的内存池 =====
        // 同一分片内由分片锁保证串行访问，所以 ObjectPool 不需要内部锁
        // 不同分片使用不同的 ObjectPool，天然无竞争
        ObjectPool<HashTableNode> node_pool;
    };

    std::vector<Shard> shards_;
    std::hash<std::string> hasher_;
    int getShardIndex(int bucket_index) const;
    int getBucketIndex(const std::string& key) const;
    std::atomic<int> size_;   // 原子计数器，避免数据竞争
};

#endif