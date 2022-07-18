# TinyWebServer项目说明

该项目为实现Linux下的轻量级Web服务器
* 参考项目网址[qinguoyi/TinyWebServer](https://github.com/qinguoyi/TinyWebServer.git)
* 参考书籍：《Linux高性能服务器编程》  游双著

## 功能说明

* 使用**线程池 + epoll(LT和ET均实现) + 模拟Proactor模式**的并发模型
* 使用**有限状态机**解析HTTP请求报文，支持解析**GET和POST**请求
* 通过访问服务器数据库实现Web端用户**注册、登录**等功能，并能够向服务器发出**图片和视频文件**等请求
* 实现**同步/异步日志系统**，记录服务器的运行状态
* 经Webbench压力测试可以实现**上万次并发连接**级别的数据交换？