#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGI_MySQL/sql_connection_pool.h"
using namespace std;

// 定义线程池模板类
template<typename T>
class threadpool {
public:
    // thread_number是线程池中线程的数量，默认8，max_requests是请求队列中最多允许的、等待处理的请求的数量，默认10000
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request);

private:
    // 工作线程运行的函数，它不断从工作队列中取出任务并执行之
    // 这两个函数定义为private，是因为在构造函数中就被pthread_create初始化，且worker函数中调用run，保证封装性
    // 而worker定义为static是防止非静态成员函数自动传入this指针作为arg默认参数，而静态成员函数没有this指针
    static void* worker(void *arg);
    void run();

private:
    int m_thread_number;            // 定义线程池中的线程数
    int m_max_request;              // 定义请求队列中允许的最大请求数
    pthread_t *m_threads;           // 定义线程池的数组，大小为m_thread_number
    list<T*> m_workqueue;           // 定义请求队列
    locker m_queuelocker;           // 保护请求队列不被其他线程访问的互斥锁
    sem m_queuestat;                // 定义请求队列中的请求任务信号量
    bool m_stop;                    // 是否结束线程
    connection_pool *m_connPool;    // 指向数据库池的数组
};

// 注意这里报错了，因为在构造函数的形参列表中不能再有默认参数 int thread_number = 8, int max_request = 10000 了
template<typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_request) : 
m_connPool(connPool), m_thread_number(thread_number), m_max_request(max_request), m_threads(NULL), m_stop(false) {
    if (m_thread_number <= 0 || m_max_request <= 0) throw exception();
    // 初始化线程池数组，大小为m_thread_number，如果为NULL，抛出错误
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) throw exception();
    // 循环创建线程，工作函数设为worker，参数传入成员对象this指针，随后将工作线程分离，不必对线程单独进行回收
    for (int i = 0; i < m_thread_number; ++i) {
        printf("create the %dth thread\n", i);
        // 注意这里不能写成 m_threads[i] 不存在这种转换形式，会报错
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            // 注意当创建/分离出错时，要释放线程池空间，避免内存泄漏
            delete[] m_threads;
            throw exception();
        }
        if (pthread_detach(m_threads[i]) != 0) {
            delete[] m_threads;
            throw exception();  
        }
    }
}

template<typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T *request) {
    // 进入工作队列前先加锁，防止其他线程同时访问工作队列
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_request) {
        printf("Workqueue is full now, please wait\n");
        // 改动1
        m_queuelocker.unlock();
        return false;
    }
    // 加入新的任务后解锁，并将信号量+1，提示有任务需要处理
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 静态成员函数定义的时候就不用加static关键字了
template<typename T>
void* threadpool<T>::worker(void *arg) {
    // 将传入的this指针强转为线程池类变量，并调用成员方法
    threadpool *pool = (threadpool*) arg;
    pool->run();
    return pool;    // 记得返回pool指针
}

template<typename T>
void threadpool<T>::run() {
    // 当未结束进程时，进入循环体
    while (!m_stop) {
        // 注意下面两句话的顺序不能颠倒！
        // 因为如果先对工作队列加锁，再信号量-1时，如果此时队列为空，信号量无法-1
        // 则需要等待调用append函数+1，而append中需要对locker解锁，进而形成死锁
        m_queuestat.wait();     // 先对工作队列任务数-1，如果队列为空，等待append添加任务即可
        m_queuelocker.lock();   // 再对工作队列加锁
        if (m_workqueue.empty()) {
            // 如果队列为空，解锁继续等待  （其实我觉得队列不会为空？）
            m_queuelocker.unlock();
            continue;
        }
        // 若为空，从队头弹出待处理的任务并解锁
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        // 若request为空，继续循环，否则取出数据库池中的一个连接
        if (!request) continue;

        // 改动2 这里网站上代码好像和源代码不一样
        connectionRAII mysqlcon(&request->m_mysql, m_connPool);
        
        request->process();

        // request->m_mysql = m_connPool->GetConnection();
        // // 执行执行http中的process函数
        // request->process();
        // // 执行后将数据库连接放回数据库池中
        // m_connPool->ReleaseConnection(request->m_mysql);
    }
}

#endif