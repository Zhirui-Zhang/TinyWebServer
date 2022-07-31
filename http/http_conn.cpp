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
// (1)这个ev是干啥的？答：是要新注册的事件，如EPOLLIN/EPOLLOUT 
// (2)为什么不用 | EPOLLIN 答：因为放在了ev事件中
// (3)为什么最后不用设置fd为非阻塞  难道因为在之前addfd时这两个已经设置完了？
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

// 关闭当前http连接，从静态成员m_epollfd中删除当前socketfd连接，注意并不是真正移除，直接将sockfd置-1，并将用户数-1
void http_conn::close_conn(bool real_close = true) {
    // 调用之前的removefd函数
    if (real_close && m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    } 
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    HTTP_CODE read_res = process_read();
    // 如果返回NO_REQUEST，说明请求不完整，需要继续读取数据
    if (read_res == NO_REQUEST) {
        // 向内核事件表中注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    // 若读取成功，根据返回的请求报文解析结果调用process_write完成响应报文填写
    bool write_res = process_write(read_res);
    if (!write_res) {
        close_conn();
    }
    // 向内核事件表中注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

// 由主线程读取浏览器发来的数据，如果工作在ET模式下，需要一次性非阻塞地循环读取全部数据
bool http_conn::read_once() {
    // 如果已经读取数据长度大于总缓冲区长度，返回false
    if (m_read_idx >= READ_BUFFER_SIZE) return false;

    // 定义已经读取的字数遍变量
    int bytes_read = 0;
#ifdef connfdLT
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read <= 0) {
            return false;
        } else {
            m_read_idx += bytes_read;
            return true;
        }
#endif

#ifdef connfdET
    while (true) {
        // 注意参数，从m_read_buf + m_read_idx开始读，能读取长度READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            // 返回-1说明出错，但是如果对于非阻塞ET模式，EAGAIN/EWOULDBLOCK表示没有数据可以读取或者数据已经全部读取完毕
            // 此时break，epoll就能再次触发sockfd上的EPOLLIN事件，以驱动下一次读操作
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else return false;
        } else if (bytes_read == 0) {
            // 如果返回0，说明连接中断，返回false
            return false;
        } else {
            // 若读取成功，移动下一次的读取位置，进入while循环直至全部读取结束后返回true
            m_read_idx += bytes_read;
        }
    }
    return true;
#endif
}

// 响应报文写入函数，非阻塞，内部调用writev函数，由主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端
// writev函数用于在一次函数调用中写多个非连续缓冲区，此处为先发送http响应报文，再发送内存映射区的请求文件内容
bool http_conn::write() {
    // 照书上代码做了改动，增加两个计数变量，tmp表示当前writev函数写入的长度，new_add表示iovec指针偏移量
    int tmp = 0, new_add = 0;

    // 如果待发送字节数<=0，出错，重新注册读事件和EPOLLONESHOT，初始化http对象，一般不会出现这种情况
    if (m_bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    // 一次性循环写入响应报文内容
    while (true) {
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        tmp = writev(m_sockfd, m_iv, m_iv_count);
        
        if (tmp > 0) {
            // 更新已发送字节
            m_bytes_have_sent += tmp;
            // 偏移iovec指针，可能为负，m_write_idx仅仅是m_iv[0]的响应报文的长度，不包括文件正文长度
            new_add = m_bytes_have_sent - m_write_idx;
        } else if (tmp == -1) {
            // 如果返回-1，可能是主线程的写缓冲区写满了，此时更新iovec结构体的指针和长度，并重新注册写事件
            // 等待主线程中下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout）
            // 因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
            if (errno = EAGAIN) {
                if (m_bytes_have_sent >= m_iv[0].iov_len) {
                    // 说明主线程缓冲区满时，子线程的响应报文部分已经发送完了
                    // 再次响应写事件时需要接着发送内存映射文件的数据
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + new_add;
                    m_iv[1].iov_len = m_bytes_to_send;
                } else {
                    // 此时说明主线程缓冲区满时，子线程的响应报文部分还未发送完，new_add是负数
                    // 再次响应写事件时需要接着发送响应报文的部分
                    // 其实这个不太可能发生，因为响应报文就那么几句话，大部分情况是内存映射文件过大，一次性写不完，导致主线程缓冲区满
                    m_iv[0].iov_base = m_write_buf + m_bytes_have_sent;
                    m_iv[0].iov_len = m_write_idx - m_bytes_have_sent;
                }
                // 重新注册写事件，返回true
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            } else {
                // 若不是缓冲区已满，说明发送失败，取消映射，返回false
                unmap();
                return false;
            }
        }

        // 更新待发送的字节数
        m_bytes_to_send -= tmp;
        // 如果m_iv缓冲区全部发送完，取消映射，重新注册事件，并根据m_linger是否保持连接
        if (m_bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if (m_linger) {
                // 如果是长连接，再次初始化http对象，返回true，否则返回false
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

// 同步线程池初始化数据库读取表
void http_conn::initmysql_result(connection_pool *connPool) {
    // 先从连接池中取出一个mysql连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlconn(&mysql, connPool);

    // 在user表中检索username，password数据，浏览器端输入
    // mysql_query函数查询数据库中的某一个表内容，如果查询成功，返回0。如果出现错误，返回非0值。
    if (mysql_query(mysql, "SELECT username, password FROM user")) {    // 注意这里最后不用加 ; 表示结束
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数，这句话好像没啥用
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组，这句话好像也没啥用
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从已有的结果集中自动获取下一行，并将tiny_webserver.user表中存放的对应用户名和密码存入map(users)中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string tmp1(row[0]);
        string tmp2(row[1]);
        users[tmp1] = tmp2;
    }
}

// 从m_read_buf读取，处理解析http请求报文
http_conn::HTTP_CODE http_conn::process_read() {
    // 初始化从状态机的状态和http请求报文的解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE res = NO_REQUEST;
    // 定义字符串表示接下来要接收的一行数据
    char* text = NULL;

    // 第一次进入时，m_check_state是init初始化的CHECK_STATE_REQUESTLINE，所以只能是parse_line返回LINE_OK才能进入
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || 
    ((line_status = parse_line()) == LINE_OK)) {
        // get_line函数 return m_read_buf + m_start_line;  字符串遇到'\0'自动结束，截断出一行
        // m_start_line是将要读取的行在buffer中的起始位置，将该位置后面的数据赋给text
        // 此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
        text = get_line();
        // m_start_line是每一个数据行在m_read_buf中的起始位置，读取一行后需要更新
        // m_checked_idx表示从状态机在m_read_buf中已经读取的位置，理论上checked_idx > start_line
        m_start_line = m_checked_idx;

        // 主状态机的三种状态转换
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            // 解析请求行，顺利的话主状态机状态会变成CHECK_STATE_HEADER，跳出switch，再次进入while循环
            res = parse_request_line(text);
            if (res == BAD_REQUEST) return BAD_REQUEST;
            break;  // 返回NO_REQUEST的情况
        }

        case CHECK_STATE_HEADER:
        {
            // 解析请求头部，顺利的话主状态机状态会变成CHECK_STATE_CONTENT
            // 如果是GET请求报文，直接响应，若POST还需要继续break进入while循环
            // 下次循环满足m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK条件，此时发生逻辑短路，从状态机不再读取新的一行
            res = parse_header(text);
            if (res == BAD_REQUEST) return BAD_REQUEST;
            // 如果收到完整请求报文（GET方法），进行跳转到报文响应函数
            else if (res == GET_REQUEST) return do_request();
            break;  // 返回NO_REQUEST的情况
        }

        case CHECK_STATE_CONTENT:
        {
            // 解析请求体，只有POST方法不为空
            res = parse_content(text);
            // 如果收到完整请求报文（POST方法），进行跳转到报文响应函数
            if (res == GET_REQUEST) return do_request();
            // 这里很关键，解析完请求体后，意味着完成报文解析，为避免再次进入循环，应将line_status置于LINE_OPEN状态，否则发生短路
            line_status = LINE_OPEN;
            break;  // 返回NO_REQUEST的情况，但我怀疑这里真的会返回NO_REQUEST吗？line_status = LINE_OPEN这句话会执行吗？
        }
        
        default:
            // 否则主状态机中不存在该状态，返回内部错误
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 向m_write_buf写入http响应报文内容
bool http_conn::process_write(HTTP_CODE res) {
    switch (res)
    {
    // 200 文件存在
    case FILE_REQUEST: {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size == 0) {
            // 如果请求文件为空，返回空白的html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string)) return false;
            return true;
        } else {
            // 请求文件非空时
            add_headers(m_file_stat.st_size);
            // m_iv[0]指向响应报文缓冲区
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // m_iv[1]指向请求文件映射区，长度为文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // 待发送数据长度等于写缓冲区字节数+文件大小
            m_bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        break;
    }
    // 403 资源无权限访问，不可读
    case FORBIDDEN_REQUEST: {
        // 添加响应报文请求行、请求头、请求体
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form)) return false;
        break;
    }
    // 404 报文语法错误
    case BAD_REQUEST: {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form)) return false;
        break;
    }
    // 500 内部错误
    case INTERNAL_ERROR: {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form)) return false;
        break;
    }   
    // 其他状态默认直接返回false即可
    default: 
        return false;
    }

    // 能运行到这里的只有403 404 500，或者200 且请求文件为空，这些状态只能申请一个iovec，指向响应报文缓冲区
    // 结构体iovec中的iov_base指向数据的地址
    m_iv[0].iov_base = m_write_buf;
    // 结构体iovec中的iov_len表示数据的长度
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 主状态机解析报文的请求行数据，获得请求方法，目标url及http版本号，例：
// GET /562f25980001b1b106000338.jpg HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // strpbrk函数是在源字符串（s1）中找出最先含有搜索字符串（s2）中“任一字符”的位置并返回，若找不到则返回空指针
    // 而http请求行各个部分是通过空格或者\t分隔的，用strpbrk函数找到任一字符出现的位置
    // 本例中 m_url = " /562f25980001b1b106000338.jpg HTTP/1.1"
    m_url = strpbrk(text, " \t");
    // 第一行中若没有，返回错误
    if (!m_url) return BAD_REQUEST;
    // 将m_url的位置置为'\0'后++，便于之后截取，此时m_url = "/562f25980001b1b106000338.jpg HTTP/1.1"
    *m_url++ = '\0';
    // 直接截取text中第一个\0的位置，比较是哪种请求方法
    char* method = text;
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else if (strcasecmp(method, "GET") == 0) {
        m_method = POST;
        m_cgi = 1;
    }
    else return BAD_REQUEST;

    // strspn函数从m_url一个一个向后查，找包含在" \t"中的字符数，实际上并没有，返回0
    m_url += strspn(m_url, " \t");
    // 同理截取版本号，此时m_version = " HTTP/1.1"
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    // 仅支持1.1版本，其他返回错误
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    // 有时url还会有http://  https:// 等特殊情况，需要单独处理，strncasecmp函数可指定比较长度
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        // 判断 / 是否是 m_url 的子串，此时m_url = "/562f25980001b1b106000338.jpg\0HTTP/1.1"
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般情况下不会有http://  https:// 判断初始是不是 / 即可
    if (!m_url || m_url[0] != '/') return BAD_REQUEST;
    // 当m_url是'/'时，拼接字符串，显示初始欢迎界面 /homepage.html
    if (strlen(m_url) == 1) strcat(m_url, "homepage.html");

    // 至此请求行处理完毕，最后记得将主状态机的状态转换为下一状态：解析头部
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 主状态机解析报文的请求头部数据，仅尝试分析几个关键字段，其他字段记录在日志中
http_conn::HTTP_CODE http_conn::parse_header(char *text) {
    // 若当前行为空行，判断接下来是否有请求体，若有说明是POST请求，否则GET请求直接返回进行do_request处理即可
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            // 若请求体长度不为0，说明是POST，转换主状态机状态，返回NO_REQUEST结果
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        } else {
            // 否则是GET请求，返回该结果
            return GET_REQUEST;
        }
    }

    else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理host字段  Host:www.wrox.com
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }

    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理Content-Length字段  Content-Length:40
        text += 15;
        text += strspn(text, " \t");
        m_content_length = stoi(text);
    }

    else if (strncasecmp(text, "Connection:", 11) == 0) {
        // 处理Connection字段  Connection:Keep-Alive
        text += 11;
        text += strspn(text, " \t");
        // 如果是长连接，将m_linger设置为true
        if (strcasecmp(text, "keep-alive") == 0) m_linger = true;
    }

    else {
        // printf("Oops! That's a unknow header: %s\n", text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }

    return NO_REQUEST;
}

// 主状态机解析报文的请求体数据，仅适用于POST请求
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if (m_content_length + m_checked_idx <= m_read_idx) {
        // 将请求体的最后一位置为\0，此时text中存储用户的账号和密码信息，放入m_user_data成员变量中
        text[m_content_length] = '\0';
        m_user_data = text;
        // 交到do_request函数中
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 生成响应报文
http_conn::HTTP_CODE http_conn::do_request() {
    // 复制文件的位置为根目录地址并记录长度
    strcpy(m_real_file, doc_root);
    int len = strlen(m_real_file);

    // strrchr函数是找到 / 在 m_url中出现的最后一次位置，后面就是要访问的资源页面
    const char *p = strrchr(m_url, '/');

    // 2 3分别为登录和注册校验页面，单独讨论
    if (m_cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 定义一个字符串，用来表示m_real_file后的尾缀
        char *real_url = "/";
        strcat(real_url, m_url + 2);
        strcat(m_real_file, real_url);

        // 将用户名和密码提取出来
        // 格式：  user=123&password=456
        char name[100], password[100];
        int i = 5;  // 越过 user=长度
        for (; m_user_data[i] != '&'; ++i) {
            name[i - 5] = m_user_data[i];
        }
        name[i - 5] = '\0';
        i += 10;    // 越过 &password= 长度
        int j = 0;
        for (; m_user_data[i] != '\0'; ++i, ++j) {
            password[j] = m_user_data[i];
        }
        password[j] = '\0';

        // 同步线程登录校验
        if (*(p + 1) == '3') {
            // 如果是注册校验，先检查是否有重名，若没有，再进行注册
            char *sql_insert = NULL;
            strcat(sql_insert, "INSERT INTO user(name, password) VALUES (");
            strcat(sql_insert, "'");        // 加个单引号增加搜索效率？
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");       // 注意这里最后不用加 ; 表示结束

            if (users.find(name) == users.end()) {
                // 用户不存在，加锁注册用户，保证同步
                m_lock.lock();
                // 说实话感觉下面这个判断有点鸡肋，执行insert语句，若失败返回非0值
                int res = mysql_query(m_mysql, sql_insert);
                if (res) {
                    // insert语句插入失败
                    m_lock.unlock();
                    strcpy(m_url, "registerError.html");
                } else {
                    // 插入成功，更新users map映射
                    users[name] = password;
                    m_lock.unlock();
                    strcpy(m_url, "log.html");
                }
            } else {
                // 用户已存在，注册失败
                strcpy(m_url, "registerError.html");
            }

        } else if (*(p + 1) == '2') {
            // 如果是登录校验，直接在已有的users即map集合中进行查找，并返回对应的页面
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
            } else {
                strcpy(m_url, "logError.html");
            }
        }
    }

    // 其他情况把网站目录和m_real_file进行拼接
    else if (*(p + 1) == '0') strcat(m_real_file, "/register.html");
    else if (*(p + 1) == '1') strcat(m_real_file, "/log.html");
    else if (*(p + 1) == '5') strcat(m_real_file, "/picture.html");
    else if (*(p + 1) == '6') strcat(m_real_file, "/video.html");
    else if (*(p + 1) == '7') strcat(m_real_file, "/fans.html");
    // 如果以上情况都不符合，将m_real_file与m_url进行拼接，这里的情况是welcome界面，请求服务器上的一个图片
    else strcat(m_real_file, m_url);

    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 如果函数返回值 < 0，说明资源文件不存在，返回，如果不可读，返回，如果是文件夹，返回
    if (stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE;   
    if (m_file_stat.st_mode & S_IROTH == 0) return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST;

    // 确认一切正常后，通过只读方式打开该文件，映射到内存区，注意要把void*返回类型转换为char*，最后关闭文件描述符
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    // 文件资源存在时，返回该值
    return FILE_REQUEST;
}

// 从状态机读取一行，分析该行内容，找关键换行字符"\r\n"，返回值有LINE_OK LINE_BAD LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
    char tmp;
    // 注意这里条件m_checked_idx < m_read_idx，因为m_read_idx是已经读取数据的下一个位置，m_checked_idx大小不能等于它
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        tmp = m_read_buf[m_checked_idx];
        // 如果是'\r'可能读取到完整行
        if (tmp == '\r') {
            if (m_checked_idx + 1 == m_read_idx) {
                // 若下一个字符到末尾，数据还不完整，返回LINE_OPEN
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 若下一个字符是'\n'说明读取到完整行，把这两个字符改为'\0'，方便之后截断，同时m_checked_idx+=2，到下一次读取的位置
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            } else {
                // 都不符合说明出现其他错误字符，返回LINE_BAD
                return LINE_BAD;
            }
        }
        // 如果是'\n'可能是上一次读到'\r'就结束了，数据不完整，需要判断
        if (tmp == '\n') {
            if (m_checked_idx >= 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                // 如果前一个字符是'\r'，再次置'\0'，并将m_checked_idx+=1
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            } else {
                // 否则返回LINE_BAD
                return LINE_BAD;
            }
        }
    }
    // 数据读取结束还没有'\r''\n'，说明数据还不完整，需要继续读取，返回LINE_OPEN
    return LINE_OPEN;
}

// 取消内存映射，利用munmap函数释放，记得释放后将原字符串设置为空
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 写入响应报文主函数，借助可变参数列表实现响应报文行/头部/体的不同输出格式
bool http_conn::add_response(const char *format, ...) {
    // 如果当前写位置大于缓冲区长度，返回false
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;

    // 创建可变参数列表，注意va_start必须和va_end成对使用，创建并释放列表
    va_list arg_list;
    va_start(arg_list, format);
    // 调用vsnprintf，功能和snprintf相同，都是输出长度size的字符到m_write_buf中，只不过vsnprintf可输出可变参数列表
    // size是当前写缓冲区的剩余空间，WRITE_BUFFER_SIZE - m_write_idx - 1
    int len = vsnprintf(m_write_buf, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list);
    if (len >= WRITE_BUFFER_SIZE - m_write_idx - 1) {
        // 如果输出长度大于size，返回false
        va_end(arg_list);
        return false;
    }
    // 记得更新写位置
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

// 添加响应报文状态行，注意可变参数列表的写法，和正常printf输出格式相同
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
}

// 添加响应报文状态头部，由于头部包含不同信息，封装到一个函数中，个人改写了一下
bool http_conn::add_headers(int content_length) {
    if (!add_content_length(content_length) || !add_linger() || 
        !add_content_type() || !add_blank_line()) return false;
    return true;    
}

// 添加响应报文状态头部中的响应报文长度信息，源代码中Content-Length:后面好像少了个空格
bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length: %d\r\n", content_length);
}

// 添加响应报文状态头部中的连接状态
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", m_linger ? "Keep-Alive" : "Close");
}

// 新增函数，添加响应报文状态头部中的响应文本类型，这里是html文本类型
bool http_conn::add_content_type() {
    return add_response("Content-Type: text/html\r\n");
} 

// 添加空白行，不知道不写%s直接返回\r\n行不行，个人觉得可以
bool http_conn::add_blank_line() {
    return add_response("\r\n");
}

// 添加响应报文体
bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}