#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define MYDEV_MAGIC 'k'
#define MYDEV_CLEAR   _IO(MYDEV_MAGIC, 0)
#define MYDEV_GETSIZE _IOR(MYDEV_MAGIC, 1, int)

int main()
{
    int fd = open("/dev/mydev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 写数据
    char *msg = "hello ioctl";
    write(fd, msg, 11);
    printf("wrote: %s\n", msg);

    // 查询数据长度
    int size = 0;
    ioctl(fd, MYDEV_GETSIZE, &size);
    printf("buffer size: %d\n", size);

    // 清空缓冲区
    ioctl(fd, MYDEV_CLEAR, 0);
    printf("buffer cleared\n");

    // 再次查询
    ioctl(fd, MYDEV_GETSIZE, &size);
    printf("buffer size after clear: %d\n", size);

    close(fd);
    return 0;
}
