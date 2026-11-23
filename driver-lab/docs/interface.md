# kqueue_lab 接口约定

## 设备模型

`/dev/kqueue_lab` 是容量为 4096 字节的共享字节 FIFO。所有打开者访问同一队列；读取会消费数据，不提供广播语义，也不保存一次 `write()` 的消息边界。

## read

- `count == 0` 返回 0，不访问 FIFO。
- FIFO 有数据时，最多返回 `min(count, 当前数据量)` 字节。
- FIFO 为空且设置 `O_NONBLOCK` 时返回 `-1/EAGAIN`。
- FIFO 为空且未设置 `O_NONBLOCK` 时进入可中断等待；被信号打断时，用户态通常观察到 `-1/EINTR`。
- 用户缓冲区完全不可写且没有复制任何字节时返回 `-1/EFAULT`。
- 用户缓冲区只允许部分复制时返回实际复制字节数；已复制数据从 FIFO 消费，并唤醒等待空间的写者。

## write

- `count == 0` 返回 0，不访问 FIFO。
- FIFO 有空间时，最多返回 `min(count, 当前剩余空间)` 字节。
- FIFO 已满且设置 `O_NONBLOCK` 时返回 `-1/EAGAIN`。
- FIFO 已满且未设置 `O_NONBLOCK` 时进入可中断等待；被信号打断时，用户态通常观察到 `-1/EINTR`。
- 用户缓冲区完全不可读且没有复制任何字节时返回 `-1/EFAULT`。
- 用户缓冲区只允许部分复制时返回实际复制字节数；已复制数据进入 FIFO，并唤醒读者、poll 等待者和 SIGIO 订阅者。

一次读写允许部分完成。用户程序需要循环处理返回字节数，不能假定请求长度等于实际长度。

## poll

- FIFO 非空：`EPOLLIN | EPOLLRDNORM`。
- FIFO 未满：`EPOLLOUT | EPOLLWRNORM`。
- 空 FIFO 仍可写；满 FIFO 仍可读。
- 就绪状态只是查询时刻的快照，不为调用者预留数据或空间。

## SIGIO/fasync

用户程序使用 `F_SETOWN` 指定接收者，并通过 `O_ASYNC` 订阅。成功写入至少一个字节后，驱动发送 `SIGIO/POLL_IN`。信号只表示设备可能可读，不携带数据；普通信号可能合并，收到信号后仍需调用 `read()` 并处理 `EAGAIN`。

关闭文件时，`.release` 会撤销该打开文件的异步通知订阅。

## ioctl

共享头文件 `include/kqueue_lab_ioctl.h` 定义：

```c
struct kqueue_info {
    __u32 capacity;
    __u32 used;
    __u32 available;
};
```

`KQUEUE_IOC_GET_INFO` 返回查询时刻的 FIFO 状态快照，不消费数据，也不预留空间。正常结果满足：

```text
used + available == capacity
```

未知命令返回 `ENOTTY`，结果地址不可写且没有复制成功时返回 `EFAULT`。

## 并发与生命周期

- mutex 保护 FIFO 内容和读写位置。
- 等待前必须释放 mutex，否则生产者或消费者无法改变条件。
- 被唤醒后必须重新加锁检查；通知不代表资源已为当前进程预留。
- `.owner = THIS_MODULE` 使打开文件持有模块引用，设备使用期间普通卸载会被拒绝。
- 模块退出时撤销 miscdevice 注册；FIFO、mutex 和等待队列均为静态对象，无动态内存释放路径。
