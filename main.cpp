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
#define MIN_TIMESLOT 5            // 最小超时单位 5s

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
    Log::get_instance()->init("ServerLog", 2000, 800000, 8);    // 同步日志模型
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
    address.sin_addr = htonl(INADDR_ANY);
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
    addfd(epollfd, pipe[0], false);

    // 传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(MIN_TIMESLOT);

    return 0;
}