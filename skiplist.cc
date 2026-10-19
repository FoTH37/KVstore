#include "skiplist.h"
#include <cstring>

const double SkipList::P = 0.5;

SkipList::SkipList() : head_(nullptr), max_level_(1), size_(0) {
    head_ = new Node("", "", MAX_LEVEL);
}

SkipList::~SkipList() {
    Node* cur = head_->forward[0];
    while (cur != nullptr) {
        Node* next = cur->forward[0];
        delete cur;
        cur = next;
    }
    delete head_;
}

int SkipList::randomLevel() {
    int level = 1;
    while ((double)rand() / RAND_MAX < P && level < MAX_LEVEL) {
        level++;
    }
    return level;
}

bool SkipList::set(const std::string& key, const std::string& value) {
    std::vector<Node*> update(MAX_LEVEL, nullptr);
    Node* cur = head_;

    // 从最高层开始查找
    for (int i = max_level_ - 1; i >= 0; i--) {
        while (cur->forward[i] != nullptr && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }

    cur = cur->forward[0];

    // 如果 key 已存在，更新 value
    if (cur != nullptr && cur->key == key) {
        cur->value = value;
        return true;
    }

    // 插入新节点
    int level = randomLevel();
    if (level > max_level_) {
        for (int i = max_level_; i < level; i++) {
            update[i] = head_;
        }
        max_level_ = level;
    }

    Node* newNode = new Node(key, value, level);
    for (int i = 0; i < level; i++) {
        newNode->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = newNode;
    }

    size_++;
    return true;
}

bool SkipList::get(const std::string& key, std::string& value) {
    Node* cur = head_;

    for (int i = max_level_ - 1; i >= 0; i--) {
        while (cur->forward[i] != nullptr && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
    }

    cur = cur->forward[0];
    if (cur != nullptr && cur->key == key) {
        value = cur->value;
        return true;
    }
    return false;
}

bool SkipList::del(const std::string& key) {
    std::vector<Node*> update(MAX_LEVEL, nullptr);
    Node* cur = head_;

    for (int i = max_level_ - 1; i >= 0; i--) {
        while (cur->forward[i] != nullptr && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }

    cur = cur->forward[0];
    if (cur == nullptr || cur->key != key) {
        return false;
    }

    // 从各层链表中删除
    for (int i = 0; i < getNodeLevel(cur); i++) {
        update[i]->forward[i] = cur->forward[i];
    }

    delete cur;
    size_--;

    // 更新 max_level_
    while (max_level_ > 1 && head_->forward[max_level_ - 1] == nullptr) {
        max_level_--;
    }

    return true;
}

bool SkipList::exists(const std::string& key) {
    Node* cur = head_;

    for (int i = max_level_ - 1; i >= 0; i--) {
        while (cur->forward[i] != nullptr && cur->forward[i]->key < key) {
            cur = cur->forward[i];
        }
    }

    cur = cur->forward[0];
    return cur != nullptr && cur->key == key;
}

int SkipList::size() const {
    return size_;
}

void SkipList::scan(std::function<bool(const std::string&, const std::string&)> callback) {
    Node* cur = head_->forward[0];
    while (cur != nullptr) {
        if (!callback(cur->key, cur->value)) return;
        cur = cur->forward[0];
    }
}