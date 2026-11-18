#ifndef KQUEUE_LAB_IOCTL_H
#define KQUEUE_LAB_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct kqueue_info {
    __u32 capacity;
    __u32 used;
    __u32 available;
};

#define KQUEUE_IOC_MAGIC 'q'
#define KQUEUE_IOC_GET_INFO \
    _IOR(KQUEUE_IOC_MAGIC, 0x01, struct kqueue_info)

#endif
