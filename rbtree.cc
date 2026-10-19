#include "rbtree.h"
#include <iostream>

RBTree::RBTree() : root_(nullptr), size_(0) {}

RBTree::~RBTree() {
    clear(root_);
}

void RBTree::clear(Node* node) {
    if (node == nullptr) return;
    clear(node->left);
    clear(node->right);
    delete node;
}

// 左旋
void RBTree::rotateLeft(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left != nullptr) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == nullptr) root_ = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

// 右旋
void RBTree::rotateRight(Node* y) {
    Node* x = y->left;
    y->left = x->right;
    if (x->right != nullptr) x->right->parent = y;
    x->parent = y->parent;
    if (y->parent == nullptr) root_ = x;
    else if (y == y->parent->left) y->parent->left = x;
    else y->parent->right = x;
    x->right = y;
    y->parent = x;
}

// 插入修复
void RBTree::fixInsert(Node* z) {
    while (z->parent != nullptr && z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            Node* y = z->parent->parent->right;
            if (y != nullptr && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotateLeft(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotateRight(z->parent->parent);
            }
        } else {
            Node* y = z->parent->parent->left;
            if (y != nullptr && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotateRight(z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rotateLeft(z->parent->parent);
            }
        }
    }
    root_->color = BLACK;
}

// 查找
RBTree::Node* RBTree::search(const std::string& key) const {
    Node* cur = root_;
    while (cur != nullptr) {
        if (key < cur->key) cur = cur->left;
        else if (key > cur->key) cur = cur->right;
        else return cur;
    }
    return nullptr;
}

bool RBTree::set(const std::string& key, const std::string& value) {
    Node* z = new Node(key, value);

    Node* y = nullptr;
    Node* x = root_;
    while (x != nullptr) {
        y = x;
        if (z->key < x->key) x = x->left;
        else if (z->key > x->key) x = x->right;
        else {
            // key 已存在，更新 value
            x->value = value;
            delete z;
            return true;
        }
    }

    z->parent = y;
    if (y == nullptr) root_ = z;
    else if (z->key < y->key) y->left = z;
    else y->right = z;

    size_++;
    fixInsert(z);
    return true;
}

bool RBTree::get(const std::string& key, std::string& value) {
    Node* node = search(key);
    if (node == nullptr) return false;
    value = node->value;
    return true;
}

bool RBTree::exists(const std::string& key) {
    return search(key) != nullptr;
}

// 找最小节点
RBTree::Node* RBTree::minimum(Node* node) const {
    while (node->left != nullptr) node = node->left;
    return node;
}

// 替换子树
void RBTree::transplant(Node* u, Node* v) {
    if (u->parent == nullptr) root_ = v;
    else if (u == u->parent->left) u->parent->left = v;
    else u->parent->right = v;
    if (v != nullptr) v->parent = u->parent;
}

// 删除修复
void RBTree::fixDelete(Node* x, Node* x_parent) {
    while (x != root_ && (x == nullptr || x->color == BLACK)) {
        if (x == x_parent->left) {
            Node* w = x_parent->right;
            if (w->color == RED) {
                w->color = BLACK;
                x_parent->color = RED;
                rotateLeft(x_parent);
                w = x_parent->right;
            }
            if ((w->left == nullptr || w->left->color == BLACK) &&
                (w->right == nullptr || w->right->color == BLACK)) {
                w->color = RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (w->right == nullptr || w->right->color == BLACK) {
                    if (w->left != nullptr) w->left->color = BLACK;
                    w->color = RED;
                    rotateRight(w);
                    w = x_parent->right;
                }
                w->color = x_parent->color;
                x_parent->color = BLACK;
                if (w->right != nullptr) w->right->color = BLACK;
                rotateLeft(x_parent);
                x = root_;
                x_parent = nullptr;
            }
        } else {
            Node* w = x_parent->left;
            if (w->color == RED) {
                w->color = BLACK;
                x_parent->color = RED;
                rotateRight(x_parent);
                w = x_parent->left;
            }
            if ((w->right == nullptr || w->right->color == BLACK) &&
                (w->left == nullptr || w->left->color == BLACK)) {
                w->color = RED;
                x = x_parent;
                x_parent = x->parent;
            } else {
                if (w->left == nullptr || w->left->color == BLACK) {
                    if (w->right != nullptr) w->right->color = BLACK;
                    w->color = RED;
                    rotateLeft(w);
                    w = x_parent->left;
                }
                w->color = x_parent->color;
                x_parent->color = BLACK;
                if (w->left != nullptr) w->left->color = BLACK;
                rotateRight(x_parent);
                x = root_;
                x_parent = nullptr;
            }
        }
    }
    if (x != nullptr) x->color = BLACK;
}

bool RBTree::del(const std::string& key) {
    Node* z = search(key);
    if (z == nullptr) return false;

    Node* y = z;
    Node* x = nullptr;
    Node* x_parent = nullptr;
    Color y_orig_color = y->color;

    if (z->left == nullptr) {
        x = z->right;
        x_parent = z->parent;
        transplant(z, z->right);
    } else if (z->right == nullptr) {
        x = z->left;
        x_parent = z->parent;
        transplant(z, z->left);
    } else {
        y = minimum(z->right);
        y_orig_color = y->color;
        x = y->right;
        x_parent = y;
        if (y->parent == z) {
            if (x != nullptr) x->parent = y;
            x_parent = y;
        } else {
            transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    delete z;
    size_--;

    if (y_orig_color == BLACK) {
        fixDelete(x, x_parent);
    }
    return true;
}

int RBTree::size() const {
    return size_;
}

void RBTree::scan(std::function<bool(const std::string&, const std::string&)> callback) {
    inorder(root_, callback);
}

void RBTree::inorder(Node* node, std::function<bool(const std::string&, const std::string&)>& callback) const {
    if (node == nullptr) return;
    inorder(node->left, callback);
    if (!callback(node->key, node->value)) return;
    inorder(node->right, callback);
}