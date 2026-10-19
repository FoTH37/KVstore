#include "hashtable.h"

HashTable::HashTable()
    : buckets_(BUCKET_COUNT, nullptr), shards_(SHARD_COUNT), size_(0) {
}

HashTable::~HashTable() {
    // 释放所有节点（每个分片用各自的内存池释放）
    for (auto& head : buckets_) {
        // 通过 bucket 索引找分片
        int bucket_idx = &head - &buckets_[0];
        int shard_idx = getShardIndex(bucket_idx);
        HashTableNode* cur = head;
        while (cur) {
            HashTableNode* next = cur->next;
            shards_[shard_idx].node_pool.deallocate(cur);
            cur = next;
        }
    }
}

int HashTable::getBucketIndex(const std::string& key) const {
    return hasher_(key) % BUCKET_COUNT;
}

int HashTable::getShardIndex(int bucket_index) const {
    return bucket_index / (BUCKET_COUNT / SHARD_COUNT);
}

bool HashTable::set(const std::string& key, const std::string& value) {
    int bucket_idx = getBucketIndex(key);
    int shard_idx = getShardIndex(bucket_idx);

    std::unique_lock<std::shared_mutex> lock(shards_[shard_idx].rwlock);

    HashTableNode* cur = buckets_[bucket_idx];
    while (cur) {
        if (cur->key == key) {
            cur->value = value;
            return true;
        }
        cur = cur->next;
    }

    // 使用该分片的内存池分配新节点
    HashTableNode* newNode = shards_[shard_idx].node_pool.allocate();
    newNode->key = key;
    newNode->value = value;
    newNode->next = buckets_[bucket_idx];
    buckets_[bucket_idx] = newNode;
    size_++;
    return true;
}

bool HashTable::get(const std::string& key, std::string& value) {
    int bucket_idx = getBucketIndex(key);
    int shard_idx = getShardIndex(bucket_idx);

    std::shared_lock<std::shared_mutex> lock(shards_[shard_idx].rwlock);

    HashTableNode* cur = buckets_[bucket_idx];
    while (cur) {
        if (cur->key == key) {
            value = cur->value;
            return true;
        }
        cur = cur->next;
    }
    return false;
}

bool HashTable::del(const std::string& key) {
    int bucket_idx = getBucketIndex(key);
    int shard_idx = getShardIndex(bucket_idx);

    std::unique_lock<std::shared_mutex> lock(shards_[shard_idx].rwlock);

    HashTableNode* prev = nullptr;
    HashTableNode* cur = buckets_[bucket_idx];
    while (cur) {
        if (cur->key == key) {
            if (prev)
                prev->next = cur->next;
            else
                buckets_[bucket_idx] = cur->next;
            shards_[shard_idx].node_pool.deallocate(cur);
            size_--;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

bool HashTable::exists(const std::string& key) {
    int bucket_idx = getBucketIndex(key);
    int shard_idx = getShardIndex(bucket_idx);

    std::shared_lock<std::shared_mutex> lock(shards_[shard_idx].rwlock);

    HashTableNode* cur = buckets_[bucket_idx];
    while (cur) {
        if (cur->key == key)
            return true;
        cur = cur->next;
    }
    return false;
}

int HashTable::size() const {
    return size_.load(std::memory_order_relaxed);
}

void HashTable::scan(std::function<bool(const std::string&, const std::string&)> callback) {
    for (auto& head : buckets_) {
        HashTableNode* cur = head;
        while (cur) {
            if (!callback(cur->key, cur->value))
                return;
            cur = cur->next;
        }
    }
}