#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <string.h>

int main()
{
    // 打开设备
    int fd = open("/dev/mydev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 创建epoll实例
    int epfd = epoll_create1(0);

    // 注册设备fd，监听可读事件
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

    printf("waiting for data...\n");

    // 先往设备写数据，触发epoll
    char *msg = "hello epoll";
    write(fd, msg, strlen(msg));

    // epoll等待事件
    struct epoll_event events[1];
    int n = epoll_wait(epfd, events, 1, 3000);
    if (n == 0) {
        printf("timeout\n");
    } else {
        char buf[64] = {0};
        int len = read(fd, buf, sizeof(buf));
        printf("epoll triggered, read %d bytes: %s\n", len, buf);
    }

    close(epfd);
    close(fd);
    return 0;
}
