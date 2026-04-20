#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <queue>
#include <stdio.h>
#include <stdlib.h>

// 任务结构：一个函数指针 + 参数
struct Task {
    void (*func)(void* arg);
    void* arg;
};

struct ThreadPool {
    pthread_t*          threads;      // 工作线程数组
    int                 thread_count;
    std::queue<Task>    task_queue;   // 任务队列
    pthread_mutex_t     mutex;        // 保护任务队列
    pthread_cond_t      cond;         // 任务到来时唤醒工作线程
    int                 stop;         // 停止标志
};

// 工作线程函数
static void* worker(void* arg) {
    ThreadPool* pool = (ThreadPool*)arg;

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        // 队列为空就等待
        while (pool->task_queue.empty() && !pool->stop) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        if (pool->stop) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        // 取出任务
        Task task = pool->task_queue.front();
        pool->task_queue.pop();
        pthread_mutex_unlock(&pool->mutex);

        // 执行任务（在锁外执行，不阻塞其他线程取任务）
        task.func(task.arg);
    }
    return NULL;
}

// 创建线程池
ThreadPool* threadpool_create(int thread_count) {
    ThreadPool* pool = new ThreadPool();
    pool->thread_count = thread_count;
    pool->stop = 0;
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    pool->threads = new pthread_t[thread_count];
    for (int i = 0; i < thread_count; i++) {
        pthread_create(&pool->threads[i], NULL, worker, pool);
        printf("Created worker thread %d\n", i);
    }
    return pool;
}

// 提交任务
void threadpool_add(ThreadPool* pool, void(*func)(void*), void* arg) {
    Task task;
    task.func = func;
    task.arg  = arg;

    pthread_mutex_lock(&pool->mutex);
    pool->task_queue.push(task);
    pthread_cond_signal(&pool->cond);   // 唤醒一个工作线程
    pthread_mutex_unlock(&pool->mutex);
}

// 销毁线程池
void threadpool_destroy(ThreadPool* pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);  // 唤醒所有线程让它们退出
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    delete[] pool->threads;
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    delete pool;
}

#endif