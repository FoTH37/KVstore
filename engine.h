#ifndef ENGINE_H
#define ENGINE_H

#include <string>
#include <functional>
class Engine{
public:
    //虚函数（virtual function）是C++实现“多态”的一种机制。
    //“多态”的意思是：用同一种方式调用不同的对象，得到不同的行为。
    //末尾的 = 0 表示这是一个纯虚函数。纯虚函数的意思是：这个类不提供实现，你必须自己在子类里实现它
    virtual ~Engine() = default;
    virtual bool set(const std::string& key, const std::string& value) = 0;
    virtual bool get(const std::string& key, std::string& value) = 0;
    virtual bool del(const std::string& key) = 0;
    virtual bool exists(const std::string& key) = 0;
    virtual int size() const = 0;
    virtual void scan(std::function<bool(const std::string&, const std::string&)>) = 0;
};

#endif