#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#define PORT 8080
#define MAX_EVENTS 1024
#define BUF_SIZE 1024

// 把fd设置为非阻塞
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 往epoll实例里注册fd，监听可读事件
// EPOLLET = 边缘触发ET模式
void epoll_add(int epoll_fd, int fd) {
    struct epoll_event ev;
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int main() {
    // === 建立监听socket（和之前一样）===
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    // === 创建epoll实例 ===
    int epoll_fd = epoll_create1(0);

    // 把listen_fd也加入epoll，监听新连接
    set_nonblocking(listen_fd);
    epoll_add(epoll_fd, listen_fd);

    printf("epoll server listening on port %d\n", PORT);

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        // epoll_wait：阻塞等待事件，返回就绪的事件数量
        // -1 表示永久等待
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // === 新连接到来 ===
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int conn_fd = accept(listen_fd,
                                     (struct sockaddr*)&client_addr,
                                     &client_len);
                if (conn_fd < 0) continue;

                printf("New connection: fd=%d\n", conn_fd);
                set_nonblocking(conn_fd);   // 新连接也设为非阻塞
                epoll_add(epoll_fd, conn_fd); // 注册到epoll

            } else {
                // === 已有连接有数据可读 ===
                char buf[BUF_SIZE];
                // ET模式必须循环读到EAGAIN，否则剩余数据不会再通知
                while (1) {
                    int bytes = read(fd, buf, BUF_SIZE);
                    if (bytes > 0) {
                        write(fd, buf, bytes);   // echo回去
                    } else if (bytes == 0) {
                        // 客户端关闭连接
                        printf("Connection closed: fd=%d\n", fd);
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        break;
                    } else {
                        // bytes < 0
                        // EAGAIN：缓冲区读完了，正常退出循环
                        // 其他错误：关闭连接
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                            break;
                        }
                    }
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
    return 0;
}