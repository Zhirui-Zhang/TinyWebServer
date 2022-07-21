# http处理连接类

根据状态转移，通过主从状态机封装了http连接类。其中，主状态机在内部调用从状态机，从状态机将处理状态和数据传给主状态机

## 功能说明

* 客户端发出http连接请求
* 从状态机读取数据，更新自身状态和接收数据，传给主状态机
* 主状态机根据从状态机状态，更新自身状态，决定响应请求还是继续读取

这里需要注重说明do_request函数中解析m_url的部分，参考推送中的说明
> m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后的m_url有8种情况。
>
>    /
>        GET请求，跳转到judge.html，即欢迎访问页面
>
>    /0
>        POST请求，跳转到register.html，即注册页面
>
>    /1
>        POST请求，跳转到log.html，即登录页面
>
>    /2CGISQL.cgi
>        POST请求，进行登录校验
>
>        验证成功跳转到welcome.html，即资源请求成功页面
>
>        验证失败跳转到logError.html，即登录失败页面
>
>    /3CGISQL.cgi
>        POST请求，进行注册校验
>
>        注册成功跳转到log.html，即登录页面
>
>        注册失败跳转到registerError.html，即注册失败页面
>
>    /5
>        POST请求，跳转到picture.html，即图片请求页面
>
>    /6
>        POST请求，跳转到video.html，即视频请求页面
>
>    /7
>        POST请求，跳转到fans.html，即关注页面
