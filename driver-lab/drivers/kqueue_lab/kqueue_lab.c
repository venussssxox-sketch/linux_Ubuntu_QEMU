// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define KQUEUE_FIFO_SIZE 4096

static DEFINE_KFIFO(kqueue_fifo, unsigned char, KQUEUE_FIFO_SIZE);
static DEFINE_MUTEX(kqueue_fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(kqueue_read_queue);
static DECLARE_WAIT_QUEUE_HEAD(kqueue_write_queue);

static int kqueue_open(struct inode *inode, struct file *file)
{
    pr_info("kqueue_lab: device opened\n");
    return 0;
}

static int kqueue_release(struct inode *inode, struct file *file)
{
    pr_info("kqueue_lab: device released\n");
    return 0;
}

static ssize_t kqueue_read(struct file *file,
                           char __user *buf,
                           size_t count,
                           loff_t *ppos)
{
    unsigned int copied = 0;
    int ret;

    if (count == 0)
        return 0;

    for (;;) {
        if (mutex_lock_interruptible(&kqueue_fifo_lock) != 0)
            return -ERESTARTSYS;

        if (!kfifo_is_empty(&kqueue_fifo))
            break;

        if (file->f_flags & O_NONBLOCK) {
            mutex_unlock(&kqueue_fifo_lock);
            return -EAGAIN;
        }

        mutex_unlock(&kqueue_fifo_lock);

        ret = wait_event_interruptible(
            kqueue_read_queue,
            !kfifo_is_empty(&kqueue_fifo));
        if (ret != 0)
            return ret;
    }

    ret = kfifo_to_user(&kqueue_fifo, buf, count, &copied);
    mutex_unlock(&kqueue_fifo_lock);

    if (ret != 0)
        return ret;

    if (copied > 0)
        wake_up_interruptible(&kqueue_write_queue);

    pr_info("kqueue_lab: read requested=%zu, copied=%u\n",
            count, copied);
    return copied;
}
static ssize_t kqueue_write(struct file *file,
                            const char __user *buf,
                            size_t count,
                            loff_t *ppos)
{
    unsigned int copied = 0;
    int ret;

    if (count == 0)
        return 0;

    for (;;) {
        if (mutex_lock_interruptible(&kqueue_fifo_lock) != 0)
            return -ERESTARTSYS;

        if (!kfifo_is_full(&kqueue_fifo))
            break;

        if (file->f_flags & O_NONBLOCK) {
            mutex_unlock(&kqueue_fifo_lock);
            return -EAGAIN;
        }

        mutex_unlock(&kqueue_fifo_lock);

        ret = wait_event_interruptible(
            kqueue_write_queue,
            !kfifo_is_full(&kqueue_fifo));
        if (ret != 0)
            return ret;
    }

    ret = kfifo_from_user(&kqueue_fifo, buf, count, &copied);
    mutex_unlock(&kqueue_fifo_lock);

    if (ret != 0)
        return ret;

    if (copied > 0)
        wake_up_interruptible(&kqueue_read_queue);

    pr_info("kqueue_lab: write requested=%zu, copied=%u\n",
            count, copied);
    return copied;
}
static const struct file_operations kqueue_fops = {
    .owner = THIS_MODULE,
    .open = kqueue_open,
	.read = kqueue_read,
    .write = kqueue_write,
    .release = kqueue_release,
};

static struct miscdevice kqueue_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "kqueue_lab",
    .fops = &kqueue_fops,
    .mode = 0666,
};

static int __init kqueue_lab_init(void)
{
    int ret;

    ret = misc_register(&kqueue_miscdev);
    if (ret != 0) {
        pr_err("kqueue_lab: misc_register failed: %d\n", ret);
        return ret;
    }

    pr_info("kqueue_lab: registered successfully\n");
    return 0;
}

static void __exit kqueue_lab_exit(void)
{
    misc_deregister(&kqueue_miscdev);
    pr_info("kqueue_lab: unregistered successfully\n");
}

module_init(kqueue_lab_init);
module_exit(kqueue_lab_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal miscdevice queue driver skeleton");
