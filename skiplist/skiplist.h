#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "./node.h"
using namespace std;

#define STORE_FILE "/home/zzr/WebServer_KV/root/dumpFile.txt" // 定义存储和载入文件位置

mutex mtx;                  // 定义互斥锁，用于插入和删除时保证同步
string delimiter = ":";     // 定义全局变量，用于检测是否为标准输入字符串  （其实没啥用）

// 泛型定义跳表skiplist类
template<typename K, typename V>
class skiplist {
public:
    skiplist() {}
    skiplist(int level);    // 构造函数
    ~skiplist();            // 析构函数

    // 下面是对外开放的接口函数
    bool insert_element(K k, V v);  // 插入元素
    bool delete_element(K k);       // 删除元素
    bool search_element(K k);       // 查找元素 
    V search_value(K k);            // 如存在，返回元素的值
    void display_list();            // 打印跳表中的元素
    void load_file();               // 从磁盘中载入已有跳表
    void dump_file();               // 将当前跳表写入磁盘
    int size();                     // 封装返回当前元素个数element_count

private:
    node<K, V>* create_node(K k, V v, int level);   // 创建新的节点，用于insert_element函数  （其实感觉没必要？直接new就行吧）
    int get_random_level();                         // 随机判断向上扩展
    // 在load_file函数中被调用，用于解析一行中的key和value值
    void get_key_value(const string& line, string *key, string *value);  
    bool is_valid_string(const string& line);        // 判断是否是符合格式的字符串，即 key:value 形式

private:
    int element_count;      // 当前跳表中的元素总个数
    int max_level;          // 跳表最大层数
    int cur_level;          // 当前跳表层数
    node<K, V> *header;     // 跳表头节点指针
    ifstream file_reader;   // 输入文件操作符，用于loadfile
    ofstream file_writer;   // 输出文件操作符，用于dumpfile
};

template<typename K, typename V>
skiplist<K, V>::skiplist(int level) : element_count(0), cur_level(0), max_level(level){
    // 初始化头节点
    K k;
    V v;
    header = new node<K, V>(k, v, max_level);
}

template<typename K, typename V>
skiplist<K, V>::~skiplist() {
    // 析构函数，关闭两个文件处理符，释放头节点  （其他节点没释放？？）
    if (file_reader.is_open()) file_reader.close();
    if (file_writer.is_open()) file_writer.close();
    // 新增的删除整个跳表部分，避免内存泄漏
    node<K, V> *cur_node = header;
    while (cur_node) {
        node<K, V> *pre_node = cur_node;
        cur_node = cur_node->forward[0];
        delete pre_node;
    }
}

template<typename K, typename V>
bool skiplist<K, V>::insert_element(K k, V v) {
    // cout << "insert element-----------------" << endl;
    mtx.lock();
    // 同delete_element函数相同，定义一个头节点和一个节点指针数组
    node<K, V> *cur_node = header;
    node<K, V> *pre_node[max_level + 1];
    memset(pre_node, 0, sizeof(node<K, V> *) * (max_level + 1));
    for (int i = cur_level; i >= 0; --i) {
        // 从最高层依次向下搜索待插入节点的前一个结点
        while (cur_node->forward[i] && cur_node->forward[i]->get_key() < k) {
            cur_node = cur_node->forward[i];
        }
        // 每到一层遍历结束后，就让该层的pre_node数组保存当前节点
        pre_node[i] = cur_node;
    }
    // 遍历到level 0时，将指针移向下一个节点
    cur_node = cur_node->forward[0];
    if (cur_node && cur_node->get_key() == k) {
        // 说明该key值已存在，更新该节点的值，报错并返回false
        cur_node->set_value(v);
        cout << "key : " << k << " already existed, value is : " << v << " now." << endl;
        mtx.unlock();
        return false;
    } else {
        // 要么是cur_node为空，说明已经遍历到最后一个空节点，要么是key值大于k的节点，需要向上插入
        // 利用随机函数得到需要上插的高度level
        int level = get_random_level();
        if (level > cur_level) {
            // 如果上插高度大于当前层数，需要将pre_node数组的上面的部分先指向header，这样的话后面插入新节点后，header也可以直接指向该层的头节点
            for (int i = cur_level + 1; i <= level; ++i) {
                pre_node[i] = header;
            }
            // 更新当前层数高度
            cur_level = level;
        }
        // 新建需要插入的节点，其实直接new node<K, V>(k, v, level)也行吧？
        node<K, V> *new_node = create_node(k, v, level);
        for (int i = 0; i <= level; ++i) {
            // 这里很关键，由于上面的层数可能cur_node并没有指向，所以借用pre_node[i]->forward[i]作为new_node的下一个节点
            new_node->forward[i] = pre_node[i]->forward[i];
            pre_node[i]->forward[i] = new_node;
        }
    }
    // 运行成功后，将跳表中元素个数-1，解锁并返回true
    cout << "key : " << k << " , value : " << v << " was successfully inserted! " << endl;
    ++element_count;
    mtx.unlock();
    return true;
}

template<typename K, typename V>
bool skiplist<K, V>::delete_element(K k) {
    cout << "delete element-----------------" << endl;
    // 注意删除节点之前先加锁，保证资源同步
    mtx.lock();
    // 定义一个新的头节点和指针数组pre_node，大小为max_level + 1，用于更新删除后的节点指向
    node<K, V> *cur_node = header;
    node<K, V> *pre_node[max_level + 1];
    memset(pre_node, 0, sizeof(node<K, V> *) * (max_level + 1));
    for (int i = cur_level; i >= 0; --i) {
        // 从最高层依次向下搜索待删除节点的前一个结点
        while (cur_node->forward[i] && cur_node->forward[i]->get_key() < k) {
            cur_node = cur_node->forward[i];
        }
        // 每到一层遍历结束后，就让该层的pre_node数组保存当前节点
        pre_node[i] = cur_node;
    }
    // 遍历到level 0时，将指针移向下一个节点
    cur_node = cur_node->forward[0];
    if (cur_node && cur_node->get_key() == k) {
        // 若节点正确，逐层向上删除
        for (int i = 0; i <= cur_level; ++i) {
            if (pre_node[i]->forward[i] != cur_node) {
                // 如果到某一层发现pre_node数组的下一个节点不是cur_node了，说明已经把该节点上面包含的所有层清空，此时退出循环
                break;
            } else {
                // 改变节点指向，即删除节点的操作  （但是并没有delete释放cur_node节点？）
                pre_node[i]->forward[i] = cur_node->forward[i];
            }
        }
        // 待所有层都指向正确节点后，删除cur_node
        delete cur_node;
        // 待删除操作结束后，查看最高层是否已经为空，若为空将cur_level减1
        while (cur_level > 0 && !header->forward[cur_level]) --cur_level;
    } else {
        // 若下个节点不是待删除的节点，说明该key值不存在，返回false
        cout << "key : " << k << " does not exist." << endl;
        mtx.unlock();
        return false;
    }
    cout << "key : " << k << " was successfully deleted! " << endl;
    // 最后记得将总元素个数减1，解锁并返回true
    --element_count;
    mtx.unlock();
    return true;
}

template<typename K, typename V>
bool skiplist<K, V>::search_element(K k) {
    // cout << "search element-----------------" << endl;
    node<K, V> *cur_node = header;
    for (int i = cur_level; i >= 0; --i) {
        // 从左上角的头部节点开始遍历寻找，如果当前节点存在且key值小于k时继续遍历
        while (cur_node->forward[i] && cur_node->forward[i]->get_key() < k) {
            cur_node = cur_node->forward[i];
        }
    }
    // 当上面循环结束时，应退出到level 0且下一个节点是key值的位置（存在的话），移动到下一个节点
    cur_node = cur_node->forward[0];
    // 如果节点存在且key值等于k，返回true，否则false
    if (cur_node && cur_node->get_key() == k) {
        cout << "Found key : " << k << ", value is : " << cur_node->get_value() << endl;
        return true;
    } else {
        cout << "Failed to find key : " << k << endl;
        return false;
    }
}

template<typename K, typename V>
V skiplist<K, V>::search_value(K k) {
    node<K, V> *cur_node = header;
    for (int i = cur_level; i >= 0; --i) {
        while (cur_node->forward[i] && cur_node->forward[i]->get_key() < k) {
            cur_node = cur_node->forward[i];
        }
    }
    cur_node = cur_node->forward[0];    
    return cur_node->get_value();
}

template<typename K, typename V>
void skiplist<K, V>::display_list() {
    cout << "Skip List-----------------" << endl;
    // 如果跳表为空，输出提示并返回
    if (!header->forward[0]) {
        cout << "<empty list>" << endl;
        return ;
    }
    // 从level 0开始逐行向上遍历，一直到cur_level，注意这里是 <=
    for (int i = 0; i <= cur_level; ++i) {
        // 定义当前行的头节点
        node<K, V> *cur_node = header->forward[i];
        cout << "level " << i << " : " << endl;
        while (cur_node) {
            cout << cur_node->get_key() << ":" << cur_node->get_value() << " ";
            // 注意这里不好理解，第i行的元素下一个节点的位置是 forward[i]处
            cur_node = cur_node->forward[i];
        }
        // 一行遍历结束后换行
        cout << endl;
    }
}

template<typename K, typename V>
void skiplist<K, V>::load_file() {
    cout << "load_file-----------------" << endl;
    file_reader.open(STORE_FILE);
    string line;
    // 定义即将载入的key和value值，用指针定义是为了方便后边截取赋值  （但是key和value new后没delete？）
    string *key = new string();
    string *value = new string();
    while (getline(file_reader, line)) {
        // 从文件中读取一行并进行判断，如果是空直接返回，否则插入到跳表中
        get_key_value(line, key, value);
        if (key->empty() || value->empty()) continue;
        insert_element(*key, *value);
        // cout << "key:" << *key << " value:" << *value << endl; 
    }
    cout << "load file \"" << STORE_FILE << "\" successed." << endl; 
    delete key;
    delete value;
    file_reader.close();
}

template<typename K, typename V>
void skiplist<K, V>::dump_file() {
    cout << "dump_file-----------------" << endl;
    file_writer.open(STORE_FILE);
    // 定义一个header头节点，将level 0层的所有节点以 key:value\n 的格式依次输出到文件中
    node<K, V> *cur_node = header->forward[0];
    while (cur_node) {
        file_writer << cur_node->get_key() << ":" << cur_node->get_value() << "\n";
        // cout << cur_node->get_key() << ":" << cur_node->get_value() << endl;
        cur_node = cur_node->forward[0];
    }
    cout << "dump file successed, data is saved into \"" << STORE_FILE << "\" now." << endl;
    file_writer.flush();
    file_writer.close();
}

template<typename K, typename V>
int skiplist<K, V>::size() {
    cout << "size of Skiplist is : " << element_count << endl;
    return element_count;
}

template<typename K, typename V>
node<K, V>* skiplist<K, V>::create_node(K k, V v, int level) {
    node<K, V> *new_node = new node<K, V>(k, v, level);
    return new_node;
}

template<typename K, typename V>
int skiplist<K, V>::get_random_level() {
    int res = 0;    // 为什么初始高度就是1呢？  （我这里改成0了）
    while (rand() % 2) ++res;
    return (res > max_level) ? max_level : res;
}

template<typename K, typename V>
void skiplist<K, V>::get_key_value(const string& line, string *key, string *value) {
    // 截取 key:value 的值，注意substr函数用法，从0开始，截取n长度，若不写n，默认到末尾
    if (!is_valid_string(line)) return ;
    *key = line.substr(0, line.find(delimiter));      // 截取str.find(delimiter)长度，正好不包括 :
    *value = line.substr(line.find(delimiter) + 1);   // 从 : 下一个位置到末尾
}  

template<typename K, typename V>
bool skiplist<K, V>::is_valid_string(const string& line) {
    // 检测是否为 key:value 格式，如果为空或者没有 : 返回false
    // 注意string中find函数的用法，如果没找到，没回的是string::npos位置
    if (line.empty() || line.find(delimiter) == string::npos) return false;
    return true;
}

#endif