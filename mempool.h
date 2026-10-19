#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <cstddef>
#include <cstdlib>
#include <vector>

// 固定大小对象池
// T: 对象类型
// BlockSize: 每批次预分配的个数
template<typename T, size_t BlockSize = 4096>
class ObjectPool {
public:
    ObjectPool() : free_head_(nullptr) {}

    ~ObjectPool() {
        for (auto* block : blocks_)
            std::free(block);
    }

    // 从池中分配一个对象（已构造）
    T* allocate() {
        if (!free_head_) {
            expandPool();
        }
        void* ptr = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        return new (ptr) T();
    }

    // 释放对象回池中（不归还系统）
    void deallocate(T* ptr) {
        ptr->~T();
        *reinterpret_cast<void**>(ptr) = free_head_;
        free_head_ = ptr;
    }

private:
    void expandPool() {
        // 每个 slot 至少能存放一个指针（链接空闲链表）
        const size_t slotSize = sizeof(T) > sizeof(void*) ? sizeof(T) : sizeof(void*);
        const size_t blockBytes = BlockSize * slotSize;
        void* block = std::malloc(blockBytes);
        if (!block) return;
        blocks_.push_back(static_cast<char*>(block));

        // 把 block 拆成 slot，串成空闲链表
        char* ptr = static_cast<char*>(block);
        for (size_t i = 0; i < BlockSize; i++) {
            *reinterpret_cast<void**>(ptr) = free_head_;
            free_head_ = ptr;
            ptr += slotSize;
        }
    }

    void* free_head_;                // 空闲链表头
    std::vector<char*> blocks_;      // 所有已分配的 block，用于析构时释放
};

#endif
