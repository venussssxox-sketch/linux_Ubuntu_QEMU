// SPDX-License-Identifier: GPL-2.0

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/signal.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include "../../include/kqueue_lab_ioctl.h"

#define KQUEUE_FIFO_SIZE 4096

static DEFINE_KFIFO(kqueue_fifo, unsigned char, KQUEUE_FIFO_SIZE);
static DEFINE_MUTEX(kqueue_fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(kqueue_read_queue);
static DECLARE_WAIT_QUEUE_HEAD(kqueue_write_queue);
static struct fasync_struct *kqueue_async_queue;

static bool kqueue_fifo_has_data(void)
{
    bool ready;

    mutex_lock(&kqueue_fifo_lock);
    ready = !kfifo_is_empty(&kqueue_fifo);
    mutex_unlock(&kqueue_fifo_lock);

    return ready;
}

static bool kqueue_fifo_has_space(void)
{
    bool ready;

    mutex_lock(&kqueue_fifo_lock);
    ready = !kfifo_is_full(&kqueue_fifo);
    mutex_unlock(&kqueue_fifo_lock);

    return ready;
}

static int kqueue_open(struct inode *inode, struct file *file)
{
    pr_info("kqueue_lab: device opened\n");
    return 0;
}

static int kqueue_fasync(int fd, struct file *file, int on)
{
    return fasync_helper(fd, file, on, &kqueue_async_queue);
}

static int kqueue_release(struct inode *inode, struct file *file)
{
    kqueue_fasync(-1, file, 0);
    pr_info("kqueue_lab: device released\n");
    return 0;
}

static __poll_t kqueue_poll(struct file *file, poll_table *wait)
{
    __poll_t mask = 0;

    poll_wait(file, &kqueue_read_queue, wait);
    poll_wait(file, &kqueue_write_queue, wait);

    mutex_lock(&kqueue_fifo_lock);
    if (!kfifo_is_empty(&kqueue_fifo))
        mask |= EPOLLIN | EPOLLRDNORM;
    if (!kfifo_is_full(&kqueue_fifo))
        mask |= EPOLLOUT | EPOLLWRNORM;
    mutex_unlock(&kqueue_fifo_lock);

    return mask;
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
            kqueue_fifo_has_data());
        if (ret != 0)
            return ret;
    }

    ret = kfifo_to_user(&kqueue_fifo, buf, count, &copied);
    mutex_unlock(&kqueue_fifo_lock);

    if (ret != 0)
        return ret;

    if (copied > 0)
        wake_up_interruptible_poll(&kqueue_write_queue,
                                   EPOLLOUT | EPOLLWRNORM);

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
            kqueue_fifo_has_space());
        if (ret != 0)
            return ret;
    }

    ret = kfifo_from_user(&kqueue_fifo, buf, count, &copied);
    mutex_unlock(&kqueue_fifo_lock);

    if (ret != 0)
        return ret;

    if (copied > 0)
        wake_up_interruptible_poll(&kqueue_read_queue,
                                   EPOLLIN | EPOLLRDNORM);

    if (copied > 0)
        kill_fasync(&kqueue_async_queue, SIGIO, POLL_IN);

    pr_info("kqueue_lab: write requested=%zu, copied=%u\n",
            count, copied);
    return copied;
}

static long kqueue_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct kqueue_info info;

    if (cmd != KQUEUE_IOC_GET_INFO)
        return -ENOTTY;

    if (mutex_lock_interruptible(&kqueue_fifo_lock) != 0)
        return -ERESTARTSYS;

    info.capacity = kfifo_size(&kqueue_fifo);
    info.used = kfifo_len(&kqueue_fifo);
    info.available = kfifo_avail(&kqueue_fifo);
    mutex_unlock(&kqueue_fifo_lock);

    if (copy_to_user((void __user *)arg, &info, sizeof(info)) != 0)
        return -EFAULT;

    return 0;
}

static const struct file_operations kqueue_fops = {
    .owner = THIS_MODULE,
    .open = kqueue_open,
    .read = kqueue_read,
    .write = kqueue_write,
    .poll = kqueue_poll,
    .unlocked_ioctl = kqueue_ioctl,
    .fasync = kqueue_fasync,
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
