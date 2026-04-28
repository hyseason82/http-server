#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hayes");
MODULE_DESCRIPTION("simple char device with poll and mutex");

#define BUF_SIZE 1024
#define MYDEV_MAGIC 'k'
#define MYDEV_CLEAR   _IO(MYDEV_MAGIC, 0)
#define MYDEV_GETSIZE _IOR(MYDEV_MAGIC, 1, int)

static char kbuf[BUF_SIZE];
static int data_len = 0;
static DECLARE_WAIT_QUEUE_HEAD(mydev_wq);
static DEFINE_MUTEX(mydev_mutex);

static int mydev_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mydev: opened\n");
    return 0;
}

static int mydev_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "mydev: closed\n");
    return 0;
}

static ssize_t mydev_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
    int n;
    if (mutex_lock_interruptible(&mydev_mutex))
        return -ERESTARTSYS;
    if (*pos >= data_len) {
        mutex_unlock(&mydev_mutex);
        return 0;
    }
    n = min((int)count, data_len - (int)*pos);
    if (copy_to_user(buf, kbuf + *pos, n)) {
        mutex_unlock(&mydev_mutex);
        return -EFAULT;
    }
    *pos += n;
    mutex_unlock(&mydev_mutex);
    printk(KERN_INFO "mydev: read %d bytes\n", n);
    return n;
}

static ssize_t mydev_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
    int n;
    if (mutex_lock_interruptible(&mydev_mutex))
        return -ERESTARTSYS;
    n = min((int)count, BUF_SIZE);
    if (copy_from_user(kbuf, buf, n)) {
        mutex_unlock(&mydev_mutex);
        return -EFAULT;
    }
    data_len = n;
    mutex_unlock(&mydev_mutex);
    printk(KERN_INFO "mydev: wrote %d bytes\n", n);
    wake_up_interruptible(&mydev_wq);
    return n;
}

static long mydev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    if (mutex_lock_interruptible(&mydev_mutex))
        return -ERESTARTSYS;
    switch (cmd) {
    case MYDEV_CLEAR:
        memset(kbuf, 0, BUF_SIZE);
        data_len = 0;
        printk(KERN_INFO "mydev: buffer cleared\n");
        break;
    case MYDEV_GETSIZE:
        if (copy_to_user((int __user *)arg, &data_len, sizeof(int)))
            ret = -EFAULT;
        printk(KERN_INFO "mydev: getsize = %d\n", data_len);
        break;
    default:
        ret = -EINVAL;
    }
    mutex_unlock(&mydev_mutex);
    return ret;
}

static __poll_t mydev_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;
    poll_wait(file, &mydev_wq, wait);
    mutex_lock(&mydev_mutex);
    if (data_len > 0)
        mask |= EPOLLIN | EPOLLRDNORM;
    mutex_unlock(&mydev_mutex);
    return mask;
}

static const struct file_operations mydev_fops = {
    .owner          = THIS_MODULE,
    .open           = mydev_open,
    .release        = mydev_release,
    .read           = mydev_read,
    .write          = mydev_write,
    .unlocked_ioctl = mydev_ioctl,
    .poll           = mydev_poll,
};

static struct miscdevice mydev_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "mydev",
    .fops  = &mydev_fops,
};

static int __init mydev_init(void)
{
    int ret = misc_register(&mydev_misc);
    if (ret) {
        printk(KERN_ERR "mydev: register failed\n");
        return ret;
    }
    printk(KERN_INFO "mydev: registered at /dev/mydev\n");
    return 0;
}

static void __exit mydev_exit(void)
{
    misc_deregister(&mydev_misc);
    printk(KERN_INFO "mydev: unregistered\n");
}

module_init(mydev_init);
module_exit(mydev_exit);