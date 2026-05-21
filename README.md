# 高并发HTTP服务器

基于 Reactor 事件驱动模型的高并发 HTTP 服务器，C++ 实现，Linux 环境。

## 技术栈

- **网络模型**：Reactor + epoll ET 边缘触发 + 非阻塞 IO
- **并发处理**：线程池（互斥锁 + 条件变量）或 SO_REUSEPORT 多线程 epoll（无锁）
- **协议解析**：有限状态机解析 HTTP/1.1 请求行与请求头
- **静态文件缓存**：启动时预构建响应缓冲区，单次 write() 无额外内存分配
- **支持方法**：GET，静态资源服务

## 压测结果

| 测试工具 | 并发数 | keep-alive | QPS | 失败请求 | 环境 |
|---|---|---|---|---|---|
| ab | 1000 | ✓ | 5500+ | 0 | Linux 6.17，loopback |

## 修复的关键 Bug

1. **ET 模式单次 read**：ET 模式必须循环 read/accept 至 EAGAIN，单次 read 丢失缓冲区剩余数据
2. **Connection 头大小写**：ab 发送 `Keep-Alive`，服务器匹配 `keep-alive` → tolower 修复
3. **SIGPIPE 未忽略**：客户端断连后 write() 触发 SIGPIPE 崩溃进程
4. **listen backlog 128**：高并发下 RST，改为 65535
5. **EPOLLERR/EPOLLHUP 未处理**：错误事件不分发到 worker 导致主线程忙轮询
6. **响应缺少 Connection 头**：HTTP/1.0 客户端未收到 `Connection: keep-alive` 则关闭连接

## 架构说明

### main.cpp — epoll + 线程池（Reactor 经典模型）

主线程通过 epoll ET 模式监听所有连接事件，有新连接或数据到来时将任务投入线程池队列，工作线程负责 HTTP 请求的解析与响应，主线程不阻塞，持续处理新事件。

### main_reuseport.cpp — SO_REUSEPORT 多线程 epoll（高吞吐）

每个线程持有独立的 listen socket 和 epoll 实例，请求在线程内部同步处理，无互斥锁/条件变量开销。适合 CPU 核数较多的场景。

## 项目结构

```

http-server/
├── src/
│   ├── main.cpp              # 主程序：epoll + 线程池
│   ├── main_reuseport.cpp    # 高吞吐版：SO_REUSEPORT 多线程 epoll
│   ├── http_conn.cpp         # HTTP 连接处理，状态机解析
│   └── threadpool.h          # 线程池实现
├── include/
│   └── http_conn.h
└── resources/

    └── index.html
```

## 运行方式

```bash
g++ -O2 -o webserver src/main.cpp src/http_conn.cpp -lpthread
# 或高吞吐版本
g++ -O2 -o webserver_rp src/main_reuseport.cpp src/http_conn.cpp -lpthread

./webserver        # 监听 8080 端口
```

## 压测命令

```bash
# keep-alive，100 并发，100000 请求
ulimit -n 65536 && ab -n 100000 -c 100 -k http://127.0.0.1:8080/
```
