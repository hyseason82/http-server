#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include "../include/http_conn.h"

#define PORT        8080
#define MAX_EVENTS  1024
#define MAX_FD      65536
// SO_REUSEPORT: N 个线程各自 accept，无锁并行处理
// 2 核机器用 2 个 epoll 线程，不做线程池，免去 mutex/cond 开销
#define THREAD_NUM  2

static HttpConn conns[MAX_FD];

static void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void epoll_add(int efd, int fd) {
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
    set_nonblocking(fd);
}

static void handle_conn(int efd, int fd) {
    HttpConn* conn = &conns[fd];

    int ret = http_conn_read(conn);
    if (ret <= 0) {
        epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }

    ret = http_conn_parse(conn);
    if (ret == 1) {
        http_conn_respond(conn);
        if (!conn->request.keep_alive) {
            epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
        } else {
            // fd stays in EPOLLET — no rearm needed; next data arrival fires it
            http_conn_init(conn, fd);
        }
    } else {
        epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
    }
}

static void* worker_thread(void* arg) {
    int listen_fd = *(int*)arg;

    int efd = epoll_create1(0);

    // 每个线程自己的 listen_fd (SO_REUSEPORT)
    epoll_add(efd, listen_fd);

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int n = epoll_wait(efd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                while (1) {
                    struct sockaddr_in ca;
                    socklen_t cl = sizeof(ca);
                    int conn_fd = accept(listen_fd, (struct sockaddr*)&ca, &cl);
                    if (conn_fd < 0) break;
                    int nodelay = 1;
                    setsockopt(conn_fd, IPPROTO_TCP, TCP_NODELAY,
                               &nodelay, sizeof(nodelay));
                    http_conn_init(&conns[conn_fd], conn_fd);
                    epoll_add(efd, conn_fd);
                }
            } else {
                handle_conn(efd, fd);
            }
        }
    }
    return NULL;
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    http_cache_init("resources");

    pthread_t threads[THREAD_NUM];
    int fds[THREAD_NUM];

    for (int i = 0; i < THREAD_NUM; i++) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        struct sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(PORT);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
        listen(lfd, 65535);
        set_nonblocking(lfd);

        fds[i] = lfd;
        pthread_create(&threads[i], NULL, worker_thread, &fds[i]);
    }

    printf("Server started on port %d (SO_REUSEPORT, %d threads)\n",
           PORT, THREAD_NUM);

    for (int i = 0; i < THREAD_NUM; i++)
        pthread_join(threads[i], NULL);

    return 0;
}
