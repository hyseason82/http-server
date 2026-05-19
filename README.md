# Linux字符设备驱动

基于Linux内核模块实现的字符设备驱动集，在WSL2环境下开发验证。
四个驱动各自独立，覆盖嵌入式Linux面试核心考点。

## 模块总览

| 文件 | 核心演示 | 设备节点 |
|------|---------|---------|
| `mydev.c` | file_operations / ioctl / poll / mutex | `/dev/mydev` |
| `mydev_platform.c` | platform_driver / probe-remove / workqueue / kmalloc | `/dev/mydev_plat` |
| `mydev_irq_tasklet.c` | hrtimer模拟IRQ / tasklet顶半部扩展 / workqueue底半部 | `/dev/mydev_irq` |
| `mydev_i2c.c` | i2c_driver / probe / smbus读写 / sysfs属性 | sysfs |

## 功能说明

### mydev.c — 字符设备基础
- `open` / `release` / `read` / `write`：用户态与内核态数据传输（`copy_to/from_user`）
- `ioctl`：`MYDEV_CLEAR`（清空缓冲区）、`MYDEV_GETSIZE`（查询数据长度）
- `poll`：等待队列 + epoll 监听设备可读事件
- `mutex_lock_interruptible`：并发保护，4线程读写测试无崩溃

### mydev_platform.c — platform_driver 模型
- `platform_driver` + `probe` / `remove` 生命周期
- `of_match_table`：设备树 compatible 匹配（真实硬件）
- `platform_set/get_drvdata`：probe 到 file_operations 的私有数据传递
- `kmalloc(GFP_KERNEL)`：物理连续内存，适合 DMA
- `workqueue`：write 触发 `schedule_work`，底半部异步唤醒 poll 等待者

### mydev_irq_tasklet.c — 中断顶/底半部
- `hrtimer` 每 500ms 触发一次，等价于硬件 IRQ 顶半部
- `tasklet_schedule`：从"ISR"调度 tasklet（软中断上下文，不可睡眠）
- `schedule_work`：从 tasklet 调度 workqueue（进程上下文，可睡眠）
- `atomic_t` + `poll`：新中断到达时才返回可读

**替换为真实中断只需**（注释中有完整示例）：
```c
devm_request_irq(&pdev->dev, irq, my_isr, IRQF_TRIGGER_RISING, "mydev", dev);
```

### mydev_i2c.c — I2C 从机驱动
- `i2c_driver` + `i2c_device_id` + `of_device_id`（双匹配表）
- `i2c_check_functionality`：probe 前验证适配器能力
- `i2c_smbus_read_byte/word_data`：寄存器读写
- `DEVICE_ATTR_RO` + `attribute_group`：sysfs 暴露温度数据
- `module_i2c_driver` 宏：简化 init/exit 注册

## 内存分配对比（面试高频）

| API | 物理连续 | 可用于DMA | 大小限制 | 适用场景 |
|-----|---------|----------|---------|---------|
| `kmalloc(GFP_KERNEL)` | ✅ | ✅ | ~128KB | 驱动常规分配 |
| `kmalloc(GFP_ATOMIC)` | ✅ | ✅ | ~128KB | 中断上下文 |
| `vmalloc` | ❌ | ❌ | 无限制 | 大块内存，不需DMA |
| `ioremap` | — | — | — | 映射硬件寄存器到虚拟地址 |

## 环境

- Ubuntu（服务器）
- Linux内核 6.17.0

## 编译

```bash
make
```

## 加载运行

```bash
# mydev（基础字符设备）
sudo insmod mydev.ko
sudo chmod 666 /dev/mydev

# platform driver
sudo insmod mydev_platform.ko
sudo chmod 666 /dev/mydev_plat

# 中断/tasklet/workqueue 演示
sudo insmod mydev_irq_tasklet.ko
# 每500ms计数+1，用epoll监听或直接读：
cat /dev/mydev_irq

# I2C 驱动（需要 i2c-stub 模拟控制器）
sudo modprobe i2c-stub chip_addr=0x48
sudo i2cset -y 0 0x48 0x07 0xa5          # 预设设备ID寄存器
sudo insmod mydev_i2c.ko
echo mydev_sensor 0x48 > /sys/bus/i2c/devices/i2c-0/new_device
cat /sys/bus/i2c/devices/0-0048/temperature_raw
```

## 测试程序

```bash
# 基础ioctl测试（针对 /dev/mydev）
gcc -o test test.c && ./test

# epoll测试
gcc -o test_epoll test_epoll.c && ./test_epoll

# 4线程并发读写
gcc -o test_concurrent test_concurrent.c -lpthread && ./test_concurrent
```

## 项目结构

```
mydev.c                字符设备基础（misc_register + file_operations全集）
mydev_platform.c       platform_driver模型（probe/remove + workqueue + kmalloc）
mydev_irq_tasklet.c    中断处理模式（hrtimer→tasklet→workqueue 三级链）
mydev_i2c.c            I2C从机驱动框架（i2c_driver + sysfs属性）
test.c                 ioctl测试程序
test_epoll.c           epoll测试程序
test_concurrent.c      并发读写测试程序
Makefile
```
