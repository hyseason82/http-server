#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include "threadpool.h"
#include "../include/http_conn.h"

#define PORT        8080
#define MAX_EVENTS  1024
#define MAX_FD      65536
#define THREAD_NUM  8

HttpConn conns[MAX_FD];
int epoll_fd;

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void epoll_add(int efd, int fd) {
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
    set_nonblocking(fd);
}

void epoll_del(int efd, int fd) {
    epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
    // 不close，交给handle_request去close
}

void handle_request(void* arg) {
    HttpConn* conn = (HttpConn*)arg;

    int ret = http_conn_read(conn);
    if (ret <= 0) {
        close(conn->fd);
        return;
    }

    ret = http_conn_parse(conn);
    if (ret == 1) {
        http_conn_respond(conn);
        if (!conn->request.keep_alive) {
            close(conn->fd);
        } else {
            http_conn_init(conn, conn->fd);
            epoll_add(epoll_fd, conn->fd);
        }
    } else {
        close(conn->fd);
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);  // 客户端断开时write不崩进程

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 65535);

    epoll_fd = epoll_create1(0);
    epoll_add(epoll_fd, listen_fd);
    http_cache_init("resources");
    ThreadPool* pool = threadpool_create(THREAD_NUM);

    printf("Server started on port %d\n", PORT);

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // ET模式：循环accept直到EAGAIN，否则高并发下会丢连接
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept(listen_fd,
                                         (struct sockaddr*)&client_addr,
                                         &client_len);
                    if (conn_fd < 0) break;  // EAGAIN：本轮连接已接完
                    int nodelay = 1;
                    setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY,
                               &nodelay, sizeof(nodelay));
                    http_conn_init(&conns[conn_fd], conn_fd);
                    epoll_add(epoll_fd, conn_fd);
                }

            } else {
                // EPOLLIN, EPOLLERR, EPOLLHUP — all handled by handle_request
                // MUST epoll_del first: ET won't re-fire until re-armed, prevents
                // double-dispatch and stops EPOLLERR spin loop
                epoll_del(epoll_fd, fd);
                threadpool_add(pool, handle_request, &conns[fd]);
            }
        }
    }

    threadpool_destroy(pool);
    close(epoll_fd);
    close(listen_fd);
    return 0;
}