#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>
using namespace std;

// 封装互斥锁类
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) throw exception();
    }
    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t* get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 封装条件变量类，本应该含有两个类成员，一个互斥锁和一个条件变量
// 但是为了适应日志中的循环队列，只保留一个条件变量m_cond，互斥锁由参数传入
class cond {
public:
    cond() {
        // if (pthread_mutex_init(&m_mutex, NULL) != 0) throw exception();
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            // 如果初始化条件变量出错，应销毁已初始化的互斥锁成员
            // pthread_mutex_destroy(&m_mutex);
            throw exception();
        }
    }
    ~cond() {
        // pthread_mutex_destroy(&m_mutex);
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex) {
        // 先给互斥锁上锁，然后调用wait函数会自动解锁，最后再给互斥锁解锁
        int res = 0;
        // pthread_mutex_lock(&m_mutex);
        res = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return res == 0;
    }
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }
    // 新增broadcast广播功能，用于日志的生产者消费者模型中
    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    // pthread_mutex_t m_mutex;     // 不再保留互斥锁成员
    pthread_cond_t m_cond;
};

// 封装信号量的类
class sem {
public:
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0) throw exception();
    }
    // 增加一个构造函数，可以设置初始value值，用于数据库连接池的初始化
    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) throw exception();
    }
    ~sem() {
        sem_destroy(&m_sem);
    }
    bool wait() {
        // P操作，当前信号量-1
        return sem_wait(&m_sem) == 0;
    }
    bool post() {
        // V操作，当前信号量+1
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

#endif