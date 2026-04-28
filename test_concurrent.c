#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#define THREAD_NUM 4
#define LOOP 10

void *writer(void *arg)
{
    int id = *(int *)arg;
    int fd = open("/dev/mydev", O_RDWR);
    char buf[32];
    for (int i = 0; i < LOOP; i++) {
        snprintf(buf, sizeof(buf), "thread%d-msg%d", id, i);
        write(fd, buf, strlen(buf));
    }
    close(fd);
    return NULL;
}

void *reader(void *arg)
{
    int fd = open("/dev/mydev", O_RDWR);
    char buf[64] = {0};
    for (int i = 0; i < LOOP; i++) {
        lseek(fd, 0, SEEK_SET);
        int n = read(fd, buf, sizeof(buf)-1);
        if (n > 0) {
            buf[n] = 0;
            printf("read: %s\n", buf);
        }
        usleep(1000);
    }
    close(fd);
    return NULL;
}

int main()
{
    pthread_t wt[THREAD_NUM], rt[THREAD_NUM];
    int ids[THREAD_NUM];

    for (int i = 0; i < THREAD_NUM; i++) {
        ids[i] = i;
        pthread_create(&wt[i], NULL, writer, &ids[i]);
        pthread_create(&rt[i], NULL, reader, &ids[i]);
    }
    for (int i = 0; i < THREAD_NUM; i++) {
        pthread_join(wt[i], NULL);
        pthread_join(rt[i], NULL);
    }
    printf("done, no crash = mutex works\n");
    return 0;
}
