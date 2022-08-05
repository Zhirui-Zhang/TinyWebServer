#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <cstdlib>
#include <cassert>
#include <sys/epoll.h>
#include <string>
#include "./CGI_MySQL/sql_connection_pool.h"
#include "./http/http_conn.h"
#include "./lock/locker.h"
#include "./log/log.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"

#define MAX_FD 65536              // 最大文件描述符个数
#define MAX_EVENT_NUMBER 10000    // 最大事件数
#define TIMESLOT 5            // 最小超时单位 5s

#define SYNLOG  // 同步写日志 
// #define ASYNLOG  异步写日志

#define listenfdLT      // 监听文件描述符水平触发 （阻塞）
// define listenfdET    // 监听文件描述符边缘触发（非阻塞）

// 以下三个函数在http_conn.cpp中定义了，这里显式声明一下
extern int setnonblocking(int fd);
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

// 设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 信号处理函数，向管道写端写入该函数值，传输字符类型，而非整型
void sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    // 可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int old_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = old_errno;
}

// 设置信号函数
void addsig(int sig, void (handler)(int), bool restart = true) {
    // 创建sigaction结构体变量并初始化
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    // 将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    // 执行sigaction函数，assert函数是内部判别式为false时终止程序，即如果返回值为-1报错
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时并不断出发SIGALRM信号
void timer_handler() {
    timer_lst.tick();
    alarm(TIMESLOT);
}

// 定时器回调函数，删除非活动连接在socket上的注册事件，关闭文件描述符，减少用户连接数
void cb_func(client_data* user_data) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    --http_conn::m_user_count;
    // 记录日志
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char* info) {
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[]) {
#ifdef SYNLOG 
    Log::get_instance()->init("ServerLog", 2000, 800000, 0);    // 同步日志模型
#endif

#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);    // 异步日志模型
#endif

    if (argc <= 1) {
        // 如果未输入端口号，该语句提醒输入格式为  ./server 9999
        printf("usage: ./%s port_number\n", basename(argv[0]));
        return -1;
    }

    int port = stoi(argv[1]);
    // 忽略sigpipe信号
    addsig(SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "230898", "tiny_webserver", 3306, 8);

    // 创建线程池，以http连接为模板对象
    threadpool<http_conn> *pool = NULL;
    try
    {
        // 调用threadpool构造函数，只需传入connPool一个参数即可，剩下两个有默认参数
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        return -1;
    }
    
    // 初始化http连接对象，个数为最大文件描述符数量，即每个用户都是一个http_conn指针
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    // 初始化数据读取表
    users->initmysql_result(connPool);

    // 创建监听文件描述符，采用TCP连接
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int res = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // 设置端口复用
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    res = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(res >= 0);
    res = listen(listenfd, 5);
    assert(res >= 0);

    // 创建内核事件表，把监听文件描述符添加到内核事件中，同时把http_conn的静态成员m_epollfd初始化
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    // 注意监听文件描述符不需要注册EPOLLONESHOT事件
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道，将读端注册内核读事件，写端设为非阻塞，因为send是将信息发送给套接字缓冲区，如果缓冲区满了
    // 则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    res = socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(res != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    // 传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    // 创建连接资源数组
    client_data *users_timer = new client_data[MAX_FD];
    // 超时默认false
    bool timeout = false;
    alarm(TIMESLOT);
    
    while (!stop_server) {
        // 调用epoll_wait函数，阻塞等待监控文件描述符是否有事件发生 
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (num < 0 && errno != EINTR) {
            // 如果返回变化数量为负且不是EINTR信号，出错，break，注意不是EAGAIN，导致出错
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // 对所有就绪事件进行处理
        for (int i = 0; i < num; ++i) {
            int sockfd = events[i].data.fd;

            // 如果文件描述符是监听fd，说明需要处理新到的客户连接
            if (sockfd == listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addr_len = sizeof(client_address);

#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr* )&client_address, &client_addr_len);
                if (connfd < 0) {
                    // 返回connfd出错，写入日志
                    LOG_ERROR("%s: errno is: %d", "accept error", errno);
                    continue;
                }
                // 若连接数量已达上限，显示当前服务器繁忙
                if (http_conn::m_user_count >= MAX_FD) {
                    show_error(connfd, "Internal Server Busy");
                    LOG_ERROR("%s", "Internal Server Busy");
                    continue;
                }

                // 若正常获得连接fd，利用它初始化http对象
                users[connfd].init(connfd, client_address);
                // 初始化client_data数据对应的连接资源，创建定时器临时变量，与用户数据绑定起来，最后把定时器添加到升序链表当中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                // 改动1
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                // 超时时间设为当前时间+三倍TIMESLOT
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

// 如果是边缘触发模式，每次需要把新到达的客户连接完全处理结束，在connfd < 0或连接达到上限时退出循环即可 
#ifdef listenfdET
                while (true) {
                    int connfd = accept(sockfd, (struct sockaddr* )&client_address, &client_addr_len);
                    if (connfd < 0) {
                        LOG_ERROR("%s: errno is: %d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD) {
                        show_error(connfd, "Internal Server Busy");
                        LOG_ERROR("%s", "Internal Server Busy");
                        break;
                    }

                    users[connfd].init(connfd, client_address);
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer();
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
#endif
            } 

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 处理异常事件，即发生变化的内核事件包括EPOLLRDHUP | EPOLLHUP | EPOLLERR事件
                // 服务器端关闭连接，并移除对应的定时器
                // 改动2 users[sockfd].close_conn();
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer) timer_lst.del_timer(timer);
            } 
            
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                // 处理定时器信号
                int sig = 0;
                char signals[1024];
                res = recv(pipefd[0], signals, sizeof(signals), 0);
                if (res == 0 || res == -1) continue;
                else {
                    for (int i = 0; i < res; ++i) {
                        if (signals[i] == SIGALRM) timeout = true;
                        if (signals[i] == SIGTERM) stop_server = true;
                    }
                }
            } 
            
            else if (events[i].events & EPOLLIN) {
                // 先创建一个定时器对象，便于后续定时处理
                // 改动3
                util_timer *timer = users_timer[sockfd].timer;
                // 处理客户连接上接收到的数据
                if (users[sockfd].read_once()) {
                    // 写入日志时用到了新增的get_address函数，转换成了struct sockaddr_in地址
                    LOG_INFO("deal with the clients(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 如果一次性读取浏览器发来的全部数据成功，将该事件放入线程池请求队列中
                    // 改动4
                    pool->append(users + sockfd);   // 应该和源代码 user + sockfd 一样吧？
                    
                    // 由于实现了数据传输，可以把相应的定时器向后移动3个TIMESLOT单位，调用adjust_timer函数
                    if (timer) {
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }

                } else {
                    // 如果读取数据失败，服务器端关闭连接，并移除对应的定时器
                    // 改动5 users[sockfd].close_conn();
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) timer_lst.del_timer(timer);
                }
            } 
            
            else if(events[i].events & EPOLLOUT) {
                // 先创建一个定时器对象，便于后续定时处理
                util_timer *timer = users_timer[sockfd].timer;
                // 处理客户连接写入的数据
                if (users[sockfd].write()) {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    
                    // 由于实现了数据传输，可以把相应的定时器向后移动3个TIMESLOT单位，调用adjust_timer函数
                    if (timer) {
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    // 如果写入数据失败，服务器端关闭连接，并移除对应的定时器
                    // 改动6 users[sockfd].close_conn();
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer) timer_lst.del_timer(timer);
                }
            }
        }
        
        // 在for循环遍历所有发生变化的文件描述符后，处理定时器超时事件，由于是非必要事件
        // 收到信号并不马上处理，而是在处理完所有读写事件后再进行处理
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    // 收尾工作，关闭所有已经建立的文件描述符，释放各种数组空间
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;

    return 0;
}