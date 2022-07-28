#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <list>
#include <mysql/mysql.h>
#include <string>
#include "../lock/locker.h"

using namespace std;

// 创建数据库连接池类，借助链表list构造
class connection_pool {
public:
    connection_pool();
    ~connection_pool();

    // 局部静态变量单例模式
    static connection_pool* GetInstance();
    // 初始化函数
    void init(string url, string user, string password, string databasename, int port, unsigned int maxconn);

    MYSQL* GetConnection();                 // 获取数据库连接
    bool ReleaseConnection(MYSQL* conn);    // 释放连接
    int GetFreeConn();                      // 获取当前空闲连接数m_freeConn
    void DestroyPool();                     // 销毁所有连接

private:
    unsigned int m_max_conn;                 // 最大连接数
    unsigned int m_free_conn;                // 当前空闲连接数，初始值应等于m_max_conn
    unsigned int m_cur_conn;                 // 当前已使用连接数，初始值应为0  m_maxConn = m_freeConn + m_curConn

    locker m_mutex;                         // 互斥锁保证线程同步
    list<MYSQL *> m_conn_list;              // 数据库连接池
    sem m_sem;                              // 信号量保证线程同步

    string m_url;                           // 主机地址
    string m_user;                          // 登录数据库用户名
    string m_password;                      // 登录数据库密码
    string m_database_name;                 // 登录数据库名
    int m_port;                          // 数据库端口号，默认3306
};

// 将数据库连接的获取与释放通过RAII机制封装，避免手动释放，内含一个数据库连接池和一个MYSQL二级指针
// 这里需要注意的是，在获取连接时，通过有参构造对传入的参数进行修改。
// 其中数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改
class connectionRAII {
public:
    // 使用双指针对MYSQL *conn进行修改
    connectionRAII(MYSQL **conn, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *connRAII;
    connection_pool *poolRAII;
};

#endif