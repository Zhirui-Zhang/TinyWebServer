#ifndef NODE_H
#define NODE_H

#include <cstring>

// 泛型定义跳表中的节点类
template<typename K, typename V>
class node {
public:
    node() {}
    node(K k, V v, int level);      // 构造函数
    ~node();                        // 析构函数

    K get_key() const;              // 包装返回key值
    V get_value() const;            // 包装返回value值
    void set_value(V v);            // 设置更改value值

    // 比较难理解的二级指针，这是个指针数组，里面存储的都是指向node的指针
    // 如果传入的level是本层的level值，指向横向下一个节点，如果不是本层level，指向纵向对应level层节点的下一个节点
    /* 例 header->forward[0]~header->forward[4]都是1  而30的节点n  n->forward[4] = n->forward[3] = NULL
          n->forward[2] = n->forward[1] = 50，n->forward[0] = 40
    level 4         1                                                      100
    level 3         1          10                    50           70       100                                      
    level 2         1          10         30         50           70       100                                      
    level 1         1    4     10         30         50           70       100                                           
    level 0         1    4  9  10         30   40    50     60    70       100 
    */
    node<K, V> **forward;           
    int node_level;                 // 当前节点所处的level层

private:
    K key;
    V value;
};

template<typename K, typename V>
node<K, V>::node(K k, V v, int level) : key(k), value(v), node_level(level) {
    // 初始化forward指针数组，new一个新的并memset初始化，注意 0~level 共level+1层
    forward = new node<K, V>*[node_level + 1];
    memset(forward, 0, sizeof(node<K, V> *) * (node_level + 1));     // 注意forward空间大小的计算方式
}

template<typename K, typename V>
node<K, V>::~node() {
    // 析构函数只需要释放指针数组即可
    delete[] forward;
}

template<typename K, typename V>
K node<K, V>::get_key() const {
    return key;
}

template<typename K, typename V>
V node<K, V>::get_value() const {
    return value;
}

template<typename K, typename V>
void node<K, V>::set_value(V v) {
    value = v;
}

#endif