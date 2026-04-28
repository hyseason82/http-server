# Linux字符设备驱动

基于Linux内核模块实现的字符设备驱动，在WSL2环境下开发验证。

## 功能

- open / release：设备打开与关闭
- read / write：用户态与内核态数据传输
- ioctl：控制命令（清空缓冲区、查询数据长度）
- poll：支持epoll监听设备可读事件
- 互斥锁保护：多线程并发读写安全

## 环境

- WSL2 + Ubuntu 22.04
- Linux内核 6.6.87

## 编译运行

    make
    sudo insmod mydev.ko
    sudo chmod 666 /dev/mydev

## 测试

    # 基础读写
    echo "hello" > /dev/mydev
    dd if=/dev/mydev bs=1024 count=1

    # ioctl测试
    gcc -o test test.c && ./test

    # epoll测试
    gcc -o test_epoll test_epoll.c && ./test_epoll

    # 并发测试
    gcc -o test_concurrent test_concurrent.c -lpthread && ./test_concurrent

## 项目结构

    mydev.c            驱动源码
    test.c             ioctl测试程序
    test_epoll.c       epoll测试程序
    test_concurrent.c  并发测试程序
    Makefile
