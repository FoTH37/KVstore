#ifndef RBTREE_H
#define RBTREE_H

#include <string>
#include <functional>
#include "engine.h"

class RBTree : public Engine {
public:
    RBTree();
    ~RBTree();

    bool set(const std::string& key, const std::string& value) override;
    bool get(const std::string& key, std::string& value) override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) override;
    int size() const override;
    void scan(std::function<bool(const std::string&, const std::string&)>) override;

private:
    enum Color { RED, BLACK };

    struct Node {
        std::string key;
        std::string value;
        Color color;
        Node* left;
        Node* right;
        Node* parent;

        Node(const std::string& k, const std::string& v)
            : key(k), value(v), color(RED), left(nullptr), right(nullptr), parent(nullptr) {}
    };

    Node* root_;
    int size_;

    // 辅助函数
    void rotateLeft(Node* x);
    void rotateRight(Node* y);
    void fixInsert(Node* z);
    void fixDelete(Node* x, Node* x_parent);
    void transplant(Node* u, Node* v);
    Node* minimum(Node* node) const;
    Node* search(const std::string& key) const;
    void clear(Node* node);
    void inorder(Node* node, std::function<bool(const std::string&, const std::string&)>& callback) const;
};

#endif