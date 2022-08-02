#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include <arpa/inet.h>
#include "../log/log.h"

// 前向声明，client_data结构体中需要用到，注意定时器类和用户资源结构体互相绑定，各自包含！！！
class util_timer;

struct client_data {
    // 客户端socket地址，其实这个变量没啥卵用，只在输出日志中用到过
    sockaddr_in address;
    int sockfd;
    // 定时器
    util_timer *timer;
};

// 定时器设计
class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    // 超时时间
    time_t expire;
    // 回调函数
    void (*cb_func)(client_data*);
    // 连接资源
    client_data *user_data;
    // 前向计时器
    util_timer *prev;
    // 后继计时器
    util_timer *next;
};

// 定时器容器设计
class sort_timer_lst {
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst() {
        while (head) {
            util_timer *tmp = head;
            head = head->next;
            delete tmp;
        }
    }

    // 添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer *timer) {
        if (!timer) return ;
        if (!head) {
            head = timer;
            tail = timer;
            return ;
        }
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return ;
        } 
        // 以上三种特殊情况都不成立时，调用内部私有成员函数
        add_timer(head, timer);
    }

    // 调整定时器，当任务发生变化时，调整定时器在链表中的位置
    void adjust_timer(util_timer *timer) {
        if (!timer) return ;
        util_timer *tmp = timer->next;
        // 如果tmp为空或者更改后任务超时时间仍 < tmp，则不动
        if (!tmp || timer->expire < tmp->expire) return ;
        // 如果是头部时间发生调整，移动head后调用私有add_timer
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            add_timer(head, timer);
        } else {
            // 否则一定是中间节点改变，更改左右节点连接指向后，调用私有add_timer
            timer->prev->next = tmp;
            tmp->prev = timer->prev;
            add_timer(head, timer);
        }
    }

    // 删除定时器
    void del_timer(util_timer *timer) {
        if (!timer) return ;
        // 如果仅剩一个节点，删除后重置头尾即可
        if (timer == head && timer == tail) {
            delete timer;
            head = NULL;
            tail = NULL;
            return ;
        }
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return ;
        }
        if (timer == tail) {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return ;
        }
        // 剩下的情况就是在中间节点了
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // 定时任务处理函数，使用统一事件源，SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器。
    void tick() {
        if (!head) return ;

        printf( "timer tick\n" );
        // 记入日志
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();

        // 获取系统当前时间
        time_t cur_time = time(NULL);
        util_timer *tmp = head;
        while (tmp) {
            if (tmp->expire <= cur_time) {
                // 如果超时时间小于当前时间，说明已经超时，执行定时器的回调函数
                tmp->cb_func(tmp->user_data);   
                // 令头节点后移指下一位，注意严谨，要判断head是否为空后再置空
                head = tmp->next;
                if (head) head->prev = NULL;
                // 释放tmp节点，并继续遍历执行超时判断
                delete tmp;
                tmp = head;
            } else break;   // 改动2 否则尚未超时，退出循环
        }
    }

private:
    // 内部私有成员函数add_timer
    void add_timer(util_timer *head, util_timer *timer) {
        util_timer *pre = head;
        util_timer *cur = pre->next;
        while (cur) {
            if (timer->expire < cur->expire) {
                pre->next = timer;
                timer->prev = pre;
                cur->prev = timer;
                timer->next = cur;
                return ;
            }
            pre = cur;
            cur = cur->next;
        } 
        // 如果遍历完仍未发现合适位置，置于tail即可，注意这里不能直接写成tail->next=timer
        // 因为可能此时tail并没有和head连上，只是单独一个节点，如果写成tail->next就断链了
        if (!cur) {
            pre->next = timer;
            timer->prev = pre;
            timer->next = NULL;
            tail = timer;
            return ;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif