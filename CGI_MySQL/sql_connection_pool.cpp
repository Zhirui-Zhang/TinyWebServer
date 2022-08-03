#include <cstdio>
#include <cstdlib>
#include <error.h>
#include <string.h>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool() {
    m_free_conn = 0;
    m_cur_conn = 0;
}

connection_pool::~connection_pool() {
    DestroyPool();
}

// 局部静态变量单例模式，由于是静态成员函数，只能创建静态成员并返回
connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

// 初始化函数
void connection_pool::init(string url, string user, string password, string databasename, int port, unsigned int maxconn) {
    // 注意这里不能用初始化列表赋值啊，这个是成员函数又不是构造函数，我在想什么啊啊啊啊啊
    // 即不可以后面跟着  : m_url(url), m_user(user), m_password(password)
    m_url = url;
    m_user = user;
    m_password = password;
    m_database_name = databasename;
    m_port = port;
    m_max_conn = maxconn;

    // 创建MaxConn条数据库连接，注意加锁
    m_mutex.lock();

    for (int i = 0; i < m_max_conn; ++i) {
        MYSQL *conn = NULL;
        conn = mysql_init(conn);
        if (!conn) {
            cout << "Error:" << mysql_error(conn);
            exit(-1);
        }
        // 连接mysql数据库语句，其中c_str是C++中string与C语言兼容，返回的是一个可读不可改的常指针，指向原字符数组
        conn = mysql_real_connect(conn, m_url.c_str(), m_user.c_str(), m_password.c_str(), m_database_name.c_str(), m_port, NULL, 0);
        if (!conn) {
            cout << "Error:" << mysql_error(conn);
            exit(-1);
        }
        // 更新连接池或空闲连接数量
        m_conn_list.push_back(conn);
        ++m_free_conn;
    }
    // 将信号量m_sem初始化为最大连接数，值设置为m_max_conn
    m_sem = sem(m_max_conn);
    m_mutex.unlock();
}

// 获取数据库连接，是取出数据库连接池中一个MYSQL资源，所以可用m_free_conn - 1，当前已用m_cur_conn + 1
MYSQL* connection_pool::GetConnection() {
    MYSQL *conn = NULL;
    if (m_conn_list.size() == 0) return NULL;
    // 为保证线程同步，对信号量和互斥锁依次进行操作，从链表头部取出新的连接
    m_sem.wait();
    m_mutex.lock();
    conn = m_conn_list.front();
    m_conn_list.pop_front();
    --m_free_conn;
    ++m_cur_conn;
    m_mutex.unlock();
    return conn;
}   

// 释放连接，是把已经用完的MYSQL资源放回连接池中，所以可用可用m_free_conn + 1，当前已用m_cur_conn - 1
bool connection_pool::ReleaseConnection(MYSQL* conn) {
    if (!conn) return false;
    m_mutex.lock();
    // 注意这里是放回连接池，调用push_back()
    m_conn_list.push_back(conn);
    ++m_free_conn;
    --m_cur_conn; 
    // 互斥锁解锁，信号量+1，表示增加一个MYSQL资源
    m_mutex.unlock();
    m_sem.post();
    return true;
}   

// 获取当前空闲连接数m_free_conn
int connection_pool::GetFreeConn() {
    return m_free_conn;
}

// 销毁所有连接，利用mysql_close函数，记得最后把cur和free变量置0，同时调用list的clear函数
void connection_pool::DestroyPool() {
    m_mutex.lock();
    // 改动2
    // if (m_conn_list.size() == 0) {
    //     m_mutex.unlock();
    //     return ;
    // }
    if (m_conn_list.size() > 0) {
        for (auto it = m_conn_list.begin(); it != m_conn_list.end(); ++it) {
            MYSQL *conn = *it;
            mysql_close(conn);
        }
        // for (auto& conn : m_conn_list) {
        //     mysql_close(conn);
        // }
        m_cur_conn = 0;
        m_free_conn = 0;
        m_conn_list.clear();
        m_mutex.unlock();
    }
    
    m_mutex.unlock();
}   

connectionRAII::connectionRAII(MYSQL **conn, connection_pool *connPool) {
    *conn = connPool->GetConnection();
    connRAII = *conn;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(connRAII);  
}