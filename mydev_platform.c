/*
 * mydev_platform.c — platform_driver 版本的字符设备驱动
 *
 * 相比 mydev.c（misc_register 直接注册），本文件演示：
 *   1. platform_driver 模型：probe/remove 生命周期，of_match_table 设备树匹配
 *   2. 延迟工作（workqueue）：模拟中断底半部，write 触发 → 工作队列异步处理
 *   3. kmalloc：替代静态数组，显式管理物理连续内存
 *
 * 在真实硬件上，设备树节点如下（驱动通过 compatible 字段匹配）：
 *   chardev@0 {
 *       compatible = "mydev,chardev";
 *       status = "okay";
 *   };
 *
 * WSL2 无设备树，通过 platform_device_alloc 手动创建设备触发 probe，
 * 驱动模式与真实硬件完全相同。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/of.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hayes");
MODULE_DESCRIPTION("platform_driver with workqueue and kmalloc");

#define BUF_SIZE 1024
#define MYDEV_MAGIC 'k'
#define MYDEV_CLEAR   _IO(MYDEV_MAGIC, 0)
#define MYDEV_GETSIZE _IOR(MYDEV_MAGIC, 1, int)

/* 每个设备实例的私有数据，probe 里分配，remove 里释放 */
struct mydev_data {
	char              *buf;       /* kmalloc 分配：物理连续，可用于 DMA */
	int                data_len;
	struct mutex       lock;
	wait_queue_head_t  wq;
	struct work_struct work;      /* 底半部工作项 */
	struct miscdevice  misc;
};

/*
 * 工作队列处理函数（底半部）：运行在进程上下文，可以睡眠。
 * 对比 tasklet（软中断上下文，不可睡眠）：workqueue 适合耗时操作。
 * 真实场景：IRQ 顶半部读硬件寄存器 → schedule_work → 底半部处理数据、唤醒等待者。
 */
static void mydev_work_fn(struct work_struct *work)
{
	struct mydev_data *dev = container_of(work, struct mydev_data, work);

	wake_up_interruptible(&dev->wq);
	dev_dbg(dev->misc.this_device, "work_fn: data ready, waiters woken\n");
}

/*
 * misc 框架在调用 open 时将 file->private_data 设为 miscdevice 指针，
 * 通过 container_of 还原出 mydev_data，再覆写 private_data 供后续接口直接使用。
 */
static int mydev_open(struct inode *inode, struct file *file)
{
	struct mydev_data *dev = container_of(file->private_data,
					      struct mydev_data, misc);
	file->private_data = dev;
	return 0;
}

static ssize_t mydev_read(struct file *file, char __user *buf,
			   size_t count, loff_t *pos)
{
	struct mydev_data *dev = file->private_data;
	int n;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	if (*pos >= dev->data_len) {
		mutex_unlock(&dev->lock);
		return 0;
	}
	n = min((int)count, dev->data_len - (int)*pos);
	if (copy_to_user(buf, dev->buf + *pos, n)) {
		mutex_unlock(&dev->lock);
		return -EFAULT;
	}
	*pos += n;
	mutex_unlock(&dev->lock);
	return n;
}

static ssize_t mydev_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *pos)
{
	struct mydev_data *dev = file->private_data;
	int n;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	n = min((int)count, BUF_SIZE);
	if (copy_from_user(dev->buf, buf, n)) {
		mutex_unlock(&dev->lock);
		return -EFAULT;
	}
	dev->data_len = n;
	mutex_unlock(&dev->lock);

	/* 模拟中断触发底半部：真实场景中 IRQ handler 里调用 schedule_work */
	schedule_work(&dev->work);
	return n;
}

static long mydev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mydev_data *dev = file->private_data;
	int ret = 0;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	switch (cmd) {
	case MYDEV_CLEAR:
		memset(dev->buf, 0, BUF_SIZE);
		dev->data_len = 0;
		break;
	case MYDEV_GETSIZE:
		if (copy_to_user((int __user *)arg, &dev->data_len, sizeof(int)))
			ret = -EFAULT;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&dev->lock);
	return ret;
}

static __poll_t mydev_poll(struct file *file, poll_table *wait)
{
	struct mydev_data *dev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &dev->wq, wait);
	mutex_lock(&dev->lock);
	if (dev->data_len > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	mutex_unlock(&dev->lock);
	return mask;
}

static const struct file_operations mydev_fops = {
	.owner          = THIS_MODULE,
	.open           = mydev_open,
	.read           = mydev_read,
	.write          = mydev_write,
	.unlocked_ioctl = mydev_ioctl,
	.poll           = mydev_poll,
};

/* probe：内核将驱动与设备匹配后调用，相当于设备"上线"初始化入口 */
static int mydev_probe(struct platform_device *pdev)
{
	struct mydev_data *dev;
	int ret;

	/* devm_kzalloc：内存与设备生命周期绑定，remove 时自动释放 */
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/*
	 * kmalloc(GFP_KERNEL)：分配物理连续内存（<128KB 可靠），可用于 DMA 传输。
	 * 对比 vmalloc：vmalloc 仅保证虚拟地址连续，物理不连续，不能直接 DMA。
	 * 对比 ioremap：ioremap 用于将硬件寄存器物理地址映射到内核虚拟地址。
	 */
	dev->buf = kmalloc(BUF_SIZE, GFP_KERNEL);
	if (!dev->buf)
		return -ENOMEM;

	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->wq);
	INIT_WORK(&dev->work, mydev_work_fn);

	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name  = "mydev_plat";
	dev->misc.fops  = &mydev_fops;

	ret = misc_register(&dev->misc);
	if (ret) {
		kfree(dev->buf);
		return ret;
	}

	platform_set_drvdata(pdev, dev);
	dev_info(&pdev->dev, "probed → /dev/mydev_plat\n");
	return 0;
}

/* remove：rmmod 或设备下线时调用，与 probe 对称清理 */
/* kernel 6.x 起返回类型改为 void（旧版本是 int） */
static void mydev_remove(struct platform_device *pdev)
{
	struct mydev_data *dev = platform_get_drvdata(pdev);

	cancel_work_sync(&dev->work);   /* 等待进行中的 work 完成再继续 */
	misc_deregister(&dev->misc);
	kfree(dev->buf);
	dev_info(&pdev->dev, "removed\n");
}

/* of_match_table：真实硬件通过 DT compatible 字段匹配此驱动 */
static const struct of_device_id mydev_of_match[] = {
	{ .compatible = "mydev,chardev" },
	{}
};
MODULE_DEVICE_TABLE(of, mydev_of_match);

static struct platform_driver mydev_pdrv = {
	.probe  = mydev_probe,
	.remove = mydev_remove,
	.driver = {
		.name           = "mydev_platform",
		.of_match_table = mydev_of_match,
	},
};

/*
 * WSL2 无设备树，手动分配一个 platform_device 触发 probe。
 * 真实硬件上删掉 mydev_pdev 相关代码，由内核从 DT 自动创建设备。
 */
static struct platform_device *mydev_pdev;

static int __init mydev_platform_init(void)
{
	int ret;

	ret = platform_driver_register(&mydev_pdrv);
	if (ret)
		return ret;

	mydev_pdev = platform_device_alloc("mydev_platform", -1);
	if (!mydev_pdev) {
		platform_driver_unregister(&mydev_pdrv);
		return -ENOMEM;
	}

	ret = platform_device_add(mydev_pdev);
	if (ret) {
		platform_device_put(mydev_pdev);
		platform_driver_unregister(&mydev_pdrv);
		return ret;
	}

	return 0;
}

static void __exit mydev_platform_exit(void)
{
	platform_device_unregister(mydev_pdev);
	platform_driver_unregister(&mydev_pdrv);
}

module_init(mydev_platform_init);
module_exit(mydev_platform_exit);
