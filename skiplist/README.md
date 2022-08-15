# 基于跳表实现的KV存储引擎

> [KV存储引擎项目地址](https://github.com/Zhirui-Zhang/Skiplist_zzr)

基于跳表数据结构，使用C++语言实现的键值对（Key-Value）型存储引擎，与WebServer项目结合，在登录时实现创建/核验/记录服务器用户名和密码等功能

## 功能说明
* 在初始化http_conn类中的mysql结构时，读取已存放的数据并存储在KV引擎中
* 采用POST方式访问时，调用内置插入/查找功能函数跳转对应界面
* 以key:value键值对的形式存储用户名：密码的映射关系