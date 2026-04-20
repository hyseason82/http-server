#include <stdio.h>
#include <unistd.h>
#include "threadpool.h"

// 模拟一个任务：打印自己的编号
void my_task(void* arg) {
    int id = *(int*)arg;
    printf("Task %d executed by thread %lu\n", id, pthread_self());
    delete (int*)arg;
}

int main() {
    // 创建4个工作线程的线程池
    ThreadPool* pool = threadpool_create(4);

    // 提交10个任务
    for (int i = 0; i < 10; i++) {
        int* id = new int(i);
        threadpool_add(pool, my_task, id);
    }

    sleep(1);  // 等任务执行完
    threadpool_destroy(pool);
    printf("All done\n");
    return 0;
}