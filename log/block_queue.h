#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <cstdlib>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

using namespace std;

// 定义循环队列模板类，其实就是STL中的queue容器，里面实现的都是它的功能
template <typename T>
class block_queue {
public:
    // 构造函数
    block_queue(int max_size = 1000) {
        if (max_size <= 0) exit(-1);
        m_array = new T[max_size];
        m_size = 0;
        m_max_size = max_size;
        m_front = -1;
        m_back = -1;
    }

    // 清空循环队列，其实我不太理解，这里面的元素并没有清空啊
    void clear() {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    // 析构函数，注意都要上锁，保证线程之间同步
    ~block_queue() {
        m_mutex.lock();
        // 改动1
        if (m_array) delete[] m_array;
        m_mutex.unlock();
    }

    bool full() {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool empty() {
        m_mutex.lock();
        if (m_size == 0) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool front(T &value) 
    {
        m_mutex.lock();
        if (m_size == 0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    bool back(T &value) 
    {
        m_mutex.lock();
        if (m_size == 0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    // 返回m_size的值，由于需要加锁，故借助一个额外变量tmp接收
    int size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    int max_size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    // push操作，模拟生产者，从尾部m_back处插入，注意互斥锁和条件变量的配合使用
    bool push(const T& item) {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            // 队列已满，广播消费者进行消费，解锁并返回false
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }
        // 将新产品放入 (m_back + 1) % m_maxsize 处后，广播通知消费者进行消费
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        ++m_size;
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    // pop操作，模拟消费者，从头部m_front处弹出
    bool pop(T& item) {
        m_mutex.lock();
        // 注意当队列为空时，需要while循环等待pthread_cond_wait，而不是if判断
        // 否则可能有一个生产资源唤醒多个线程，后唤醒的线程已经没有资源可用，却继续执行，导致出错
        while (m_size <= 0) {
            // 注意这里不能直接写 m_cond.wait(&m_mutex) 因为m_mutex是个locker类，并不是wait函数需要的mutex互斥锁，所以要在locker中再加一个成员函数
            if (!m_cond.wait(m_mutex.get())) {
                // 如果信号量函数返回false，说明出错，解锁互斥锁，返回false
                m_mutex.unlock();
                return false;
            }
        }
        // 若当前线程获得竞争生产资源的机会，从头部(m_front + 1) % m_maxsize处取出资源
        // 注意m_front初始为-1，表示待取出资源的前一个位置
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        --m_size;
        m_mutex.unlock();
        return true;
    }

    // 还有一个增加超时处理的pop函数，需要在cond类中新增pthread_cond_timewait()函数，这里就不列出了

private:
    locker m_mutex;
    cond m_cond;
    // 循环队列数组
    T *m_array;
    // 当前队列的大小
    int m_size;
    // 数组的最大容量
    int m_max_size;
    // 当前头部位置，用于pop
    int m_front;
    // 当前尾部位置，用于push
    int m_back;
};

#endif