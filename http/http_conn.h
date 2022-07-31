#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <cstdarg>
#include <error.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGI_MySQL/sql_connection_pool.h"

class http_conn {
public:
    // 读取文件名m_real_file的最大长度
    static const int FILENAME_LEN = 200;
    // 读缓冲区m_read_buf的长度
    static const int READ_BUFFER_SIZE = 2048;
    // 写缓冲区m_write_buf的长度
    static const int WRITE_BUFFER_SIZE = 1024;

    // http请求报文的9中请求方法，这里只用到GET和POST两种
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH};
    // 主状态机的三种状态，解析请求行（第一行）/解析请求头部/解析请求体（GET报文没有请求体）
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    // http请求报文解析结果的返回值，不知道为什么源代码中只有这个第一位没设置为0
    enum HTTP_CODE {NO_REQUEST = 0, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};   
    // 从状态机的三种状态，成功读取一行/读取失败/等待继续读取
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN}; 

public:
    // 下面定义了init和close_http函数用于类的初始化和关闭，故ctor和dtor只是声明，没有实际用处
    http_conn();
    ~http_conn();

public:
    // 初始化套接字地址，内部会调用私有成员函数init()
    void init(int sockfd, const sockaddr_in &addr);
    // 关闭http连接
    void close_conn(bool real_close = true);

    // 处理客户请求，也就是threadpool中模板类run()中调用的模板T的成员函数
    void process();
    // 一次性非阻塞地读取浏览器发来的全部数据
    bool read_once();
    // 响应报文写入函数，非阻塞
    bool write();

    // 新增的两个额外函数，这个get_address用过吗？答：在主函数中用过一次  （和公众号写的不太一样，少了一个函数）
    sockaddr_in* get_address() {return &m_address;}
    // 同步线程池初始化数据库读取表
    void initmysql_result(connection_pool *connPool);

private:
    // 内部的私有初始化调用
    void init();

    // 从m_read_buf读取，处理解析http请求报文
    HTTP_CODE process_read();
    // 向m_write_buf写入http响应报文内容
    bool process_write(HTTP_CODE res);

    // 下面四个函数被process_read调用，用来分析http请求
    // 主状态机解析报文的请求行数据，注意不要和从状态机的读取一行parse_line函数搞混了！
    HTTP_CODE parse_request_line(char *text);
    // 主状态机解析报文的请求头部数据
    HTTP_CODE parse_header(char *text);
    // 主状态机解析报文的请求体数据
    HTTP_CODE parse_content(char *text);
    // 生成响应报文
    HTTP_CODE do_request();

    // 用于将文件内容指针向后偏移，指向未处理的字符，m_start_line是已经解析的字符
    char* get_line() {return m_read_buf + m_start_line;}

    // 从状态机读取一行，分析该行内容，判断是请求报文的哪一部分
    LINE_STATUS parse_line();

    // 下面这些函数被process_write调用，用以填充http响应报文
    void unmap();

    // 下面8个函数由do_request调用，根据响应报文格式生成8个部分
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_content_type();    // 新增函数，但是源代码里没用过，离谱，我自己用吧
    bool add_blank_line();
    bool add_content(const char *content);

public:
    // 所有socket上的事件都被注册到同一个epoll内核事件表中，所以将epollfd设置为静态成员变量
    static int m_epollfd;
    // 统计用户数量
    static int m_user_count;
    // 新增的MYSQL类型成员变量
    MYSQL* m_mysql;

private:
    // 该http连接的sockfd和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;      // 这个变量其实没啥卵用
    
    // 读缓冲区的http请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    // m_read_buf数据中一次性读取数据的最后一个字节的下一个位置，应该是最大的值
    int m_read_idx;
    // m_read_buf中已经读取到的字节总数，应为是三个数中间值  Q:这三个参数的关系和区别？
    int m_checked_idx;
    // 表示即将解析的行的起始位置，应是最小值
    int m_start_line;

    // 写缓冲区的数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // m_write_buf中待发送的字节数
    int m_write_idx;

    // 主状态机当前状态
    CHECK_STATE m_check_state;
    // 请求方法类型
    METHOD m_method;

    // 客户请求目标文件的完整路径，其内容等于doc_root + m_url，doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    // 客户请求的目标文件名称
    char *m_url;
    // http版本协议号，仅支持HTTP/1.1
    char *m_version;
    // 主机名
    char *m_host;
    // http请求报文请求'体'的长度
    int m_content_length;
    // http请求是否要保持连接
    bool m_linger;
    // 客户请求的目标文件被内存映射到的起始位置
    char *m_file_address;
    // 目标文件的信息，用来判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    // 采用writev来执行写操作，故定义io向量，m_iv_count表示被写内存块的数量
    struct iovec m_iv[2];
    int m_iv_count;

    // 下面为新增变量
    // 是否启用的POST
    int m_cgi;    
    // 存储用户名和密码信息
    char *m_user_data; 
    // 剩余发送字节数，该值为响应报文长度+内存映射文件长度之和
    int m_bytes_to_send;
    // 已发送字节数
    int m_bytes_have_sent;
};

#endif