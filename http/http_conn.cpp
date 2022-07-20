#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

// 定义两种文件描述符的触发方式，如果是ET边缘触发的话，下次调用后不返回，每次必须读取完所有的数据，故fd应设置为非阻塞
#define listenfdLT      // 监听fd水平触发（阻塞）
// #define listenfdET   // 监听fd边缘触发（非阻塞）

// #define connfdLT     // 连接fd水平触发（阻塞）
#define connfdET        // 连接fd边缘触发（非阻塞）

// 定义http响应的一些常见的状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char *doc_root = "/home/zzr/TinyWebServer/root";

// 初始化两个静态成员变量
// 所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epollfd设置为静态成员变量
int http_conn::m_epollfd = -1;
// 统计用户数量
int http_conn::m_user_count = 0;

// 将表中的用户名和密码放入map，再定义一个互斥锁
map<string, string> users;
locker m_lock;

// 定义几个处理文件描述符的函数，在main函数中会用到，并借助extern关键字声明
// 1.定义文件描述符非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 2.向内核事件表中注册读事件，设置不同触发方式，以及是否开启EPOLLONESHOT，保证一个socket连接在任一时刻都只被一个线程处理
// 同时在当该线程处理完后，需要通过modfd函数中的epoll_ctl重置epolloneshot事件
void addfd(int epollfd, int fd, bool one_shot) {
    // 定义一个epoll_event结构体，包括data（含fd）和events（待注册事件）
    epoll_event event;
    event.data.fd = fd;

// 根据不同触发方式，决定是否 | EPOLLET，而EPOLLRDHUP用来判断对端是否关闭
#ifdef listenfdLT   
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

    // 还要判断是否 | EPOLLONESHOT 
    if (one_shot) event.events |= EPOLLONESHOT;
    // 向内核中注册读事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 记得将fd设为非阻塞
    setnonblocking(fd);
}

// 3.从内核事件表中移除某fd，移除后记得关闭fd
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 4.在内核事件表中对某fd进行修改，重置EPOLLONESHOT事件，有三个问题如下：
// 1.这个ev是干啥的？ 2.为什么不用 | EPOLLIN 3.为什么最后不用设置fd为非阻塞  难道因为在之前addfd时这两个已经设置完了？
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;

// 根据不同触发方式，决定是否 | EPOLLET，由于listenfd不用开启EPOLLONESHOT，也就没必要重置修改
#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdET
    event.events = ev | EPOLLONESHOT | EPOLLET | EPOLLRDHUP;
#endif

    // 重置EPOLLONESHOT事件，用EPOLL_CTL_MOD修改
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 接下来就是所有成员函数的定义部分了，下面尽量按照头文件中声明的顺序来定义
// 初始化一个新的http对象，内部会调用私有成员函数init()
void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;
    init();
}
// 私有成员函数init()
void http_conn::init() {
    m_mysql = NULL;
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;
    m_write_idx = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_content_length = 0;
    m_linger = false;
    m_cgi = 0;    
    m_bytes_to_send = 0;
    m_bytes_have_sent = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 关闭http连接，从静态成员m_epollfd中删除当前socketfd连接，注意并不是真正移除，直接将sockfd置-1，并将用户数-1
void http_conn::close_http(bool real_close = true) {
    // 调用之前的removefd函数
    if (real_close && m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    } 
}