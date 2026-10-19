#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <string>
#include <vector>
#include <functional>
#include <cstdlib>
#include "engine.h"

class SkipList : public Engine {
public:
    SkipList();
    ~SkipList();

    bool set(const std::string& key, const std::string& value) override;
    bool get(const std::string& key, std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    int size() const override;
    void scan(std::function<bool(const std::string&, const std::string&)>) override;

private:
    struct Node {
        std::string key;
        std::string value;
        std::vector<Node*> forward;  // 各层的下一个节点指针

        Node(const std::string& k, const std::string& v, int level)
            : key(k), value(v), forward(level, nullptr) {}
    };

    Node* head_;        // 头节点（哨兵，不含数据）
    int max_level_;     // 当前最大层数
    int size_;          // 元素个数

    static const int MAX_LEVEL = 16;  // 最大允许层数
    static const double P;            // 层数增长概率

    int randomLevel();
    int getNodeLevel(Node* node) const { return node->forward.size(); }
};

#endif