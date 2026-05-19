/*
 * mydev_irq_tasklet.c — 中断顶半部 / 底半部模式演示
 *
 * 真实硬件上的中断处理链：
 *   硬件触发 IRQ
 *     → 顶半部（ISR，request_irq 注册）：中断上下文，不可睡眠
 *         只做：清中断标志、读必要寄存器、tasklet_schedule 或 schedule_work
 *     → tasklet（软中断上下文，不可睡眠）：比 workqueue 延迟低，适合快速处理
 *     → workqueue（进程上下文，可睡眠）：适合耗时操作、访问用户态、I2C通信等
 *
 * WSL2 无真实硬件中断，用 hrtimer 模拟定时 IRQ，代码结构与真实驱动完全对应。
 * 若接入真实硬件，只需将 hrtimer 替换为 request_irq（见注释）。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/poll.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hayes");
MODULE_DESCRIPTION("IRQ top/bottom-half demo: hrtimer -> tasklet -> workqueue");

#define TIMER_INTERVAL_MS 500

static atomic_t irq_count  = ATOMIC_INIT(0);
static atomic_t new_data   = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(demo_wq);
static struct work_struct   demo_work;
static struct tasklet_struct demo_tasklet;
static struct hrtimer        demo_timer;

/*
 * 底半部（workqueue）：进程上下文，可睡眠。
 * 适合：耗时计算、I2C/SPI 读写、copy_to_user、mutex_lock。
 */
static void demo_work_fn(struct work_struct *work)
{
	atomic_inc(&irq_count);
	atomic_set(&new_data, 1);
	wake_up_interruptible(&demo_wq);
	pr_debug("irq_demo: workqueue done, count=%d\n", atomic_read(&irq_count));
}

/*
 * 底半部（tasklet）：软中断上下文，不可睡眠，不可用 mutex。
 * 比 workqueue 延迟更低，适合：协议帧解析、状态机推进。
 * 这里演示 tasklet 触发 workqueue 的两级底半部结构。
 *
 * 注意：kernel 5.9+ tasklet 回调改为接收 tasklet_struct 指针，
 * 用 from_tasklet 宏获取私有数据（此处无私有数据，略）。
 */
static void demo_tasklet_fn(struct tasklet_struct *t)
{
	pr_debug("irq_demo: tasklet, scheduling workqueue\n");
	schedule_work(&demo_work);
}

/*
 * 模拟 ISR 顶半部（hrtimer 回调，等价于中断上下文）：
 *   不可睡眠，不能 mutex_lock，不能 kmalloc(GFP_KERNEL)。
 *   规则：尽快完成，只做最少工作，延迟到底半部处理。
 *
 * 替换为真实中断：
 *   irq = platform_get_irq(pdev, 0);
 *   devm_request_irq(&pdev->dev, irq, my_isr, IRQF_TRIGGER_RISING, "mydev", dev);
 *
 *   static irqreturn_t my_isr(int irq, void *dev_id) {
 *       struct mydev_data *dev = dev_id;
 *       // 清中断标志（必须，否则 IRQ 持续触发）
 *       writel(1, dev->base + IRQ_CLEAR_REG);
 *       tasklet_schedule(&dev->tasklet);
 *       return IRQ_HANDLED;
 *   }
 */
static enum hrtimer_restart demo_timer_fn(struct hrtimer *timer)
{
	tasklet_schedule(&demo_tasklet);
	hrtimer_forward_now(timer, ms_to_ktime(TIMER_INTERVAL_MS));
	return HRTIMER_RESTART;
}

/* 用户读取中断计数 */
static ssize_t demo_read(struct file *file, char __user *buf,
			  size_t count, loff_t *pos)
{
	char tmp[32];
	int len = snprintf(tmp, sizeof(tmp), "irq_count=%d\n",
			   atomic_read(&irq_count));
	atomic_set(&new_data, 0);
	return simple_read_from_buffer(buf, count, pos, tmp, len);
}

/* poll：有新中断到达时才可读（演示 new_data 标志配合等待队列） */
static __poll_t demo_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &demo_wq, wait);
	if (atomic_read(&new_data))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations demo_fops = {
	.owner  = THIS_MODULE,
	.read   = demo_read,
	.poll   = demo_poll,
};

static struct miscdevice demo_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "mydev_irq",
	.fops  = &demo_fops,
};

static int __init demo_init(void)
{
	int ret;

	INIT_WORK(&demo_work, demo_work_fn);
	tasklet_setup(&demo_tasklet, demo_tasklet_fn);

	ret = misc_register(&demo_misc);
	if (ret)
		return ret;

	/* 启动定时器，模拟硬件每 500ms 产生一次中断 */
	/* kernel 6.15+ 用 hrtimer_setup 替代 hrtimer_init + 手动赋 .function */
	hrtimer_setup(&demo_timer, demo_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&demo_timer, ms_to_ktime(TIMER_INTERVAL_MS), HRTIMER_MODE_REL);

	pr_info("irq_demo: loaded, /dev/mydev_irq ready (IRQ simulated every %dms)\n",
		TIMER_INTERVAL_MS);
	return 0;
}

static void __exit demo_exit(void)
{
	hrtimer_cancel(&demo_timer);
	tasklet_kill(&demo_tasklet);
	cancel_work_sync(&demo_work);
	misc_deregister(&demo_misc);
	pr_info("irq_demo: unloaded, total simulated IRQs: %d\n",
		atomic_read(&irq_count));
}

module_init(demo_init);
module_exit(demo_exit);
