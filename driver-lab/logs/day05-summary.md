# Day 5 总结：poll、并发验证与异步通知

日期：2026-11-17—2026-11-18

## 1. 今天具体完成了什么？

- 将 Day 4 编写的模块和测试程序部署到 Buildroot 根文件系统，在 QEMU ARM64 来宾中完成阻塞读、阻塞写、非阻塞访问和信号中断的运行验证。
- 理解用户态、系统调用、VFS、poll 通用框架与具体驱动回调之间的分工，明确 VFS 和 poll 都属于内核态通用机制。
- 为 `kqueue_lab` 增加 `.poll` 回调，使用户程序可以查询 FIFO 的可读和可写状态，并在状态变化时被唤醒。
- 修正等待条件读取 FIFO 状态时未持有 mutex 的并发问题，增加带锁的状态查询助手函数。
- 完成全部既有测试的回归，确认并发修正没有破坏阻塞、非阻塞、信号中断与 poll 行为。
- 编写生产者/消费者测试，验证单生产者数据顺序、双生产者总量以及用户态对部分写入的循环处理。
- 编写错误路径测试，验证无效用户地址返回 `EFAULT`，并确认失败读取不会错误消费 FIFO 数据。
- 编写模块生命周期测试，验证设备打开期间模块引用计数阻止卸载，文件关闭后可以正常卸载。
- 为驱动加入 `.fasync` 异步通知支持，用户程序设置 `F_SETOWN` 与 `O_ASYNC` 后，FIFO 写入数据会触发 `SIGIO`。
- 编写 `SIGIO` 测试并在 QEMU ARM64 中通过，确认异步通知到达后可以读取到对应数据。

## 2. 用户态、VFS、poll 框架和驱动的关系

一次用户态 `poll()` 的主要路径是：

```text
用户程序 poll(fds, ...)
→ 系统调用进入内核态
→ Linux poll 通用框架处理 fd、事件集合和超时
→ VFS 根据 fd 找到 struct file
→ struct file 找到驱动的 file_operations
→ 调用 kqueue_poll(file, wait)
→ 驱动登记等待队列并报告 FIFO 当前状态
→ poll 框架综合所有 fd 的结果后返回用户态
```

用户态与内核态是 CPU 权限和地址空间的边界。VFS 是内核中统一文件接口的大型子系统；poll 框架是内核中协调多个文件描述符就绪事件、等待和超时的通用机制；`kqueue_lab` 是使用这些通用机制的具体字符设备驱动。

驱动的 `file_operations` 是 VFS 与设备实现之间的分派表：

```text
read()  → .read   → kqueue_read()
write() → .write  → kqueue_write()
poll()  → .poll   → kqueue_poll()
close() → .release→ kqueue_release()
O_ASYNC → .fasync → kqueue_fasync()
```

QEMU 的作用是提供 ARM64 虚拟机器。相同的驱动 C 源码由 ARM64 交叉工具链编译，在 ARM64 来宾内核中运行；它不是用 ARM64 的 `kqueue_poll()` 替换主机 x86 内核中的同名函数。

## 3. poll 回调的实现与意义

驱动新增：

```c
static __poll_t kqueue_poll(struct file *file, poll_table *wait)
```

`__poll_t` 是就绪事件位掩码类型。一个返回值可以同时包含多个事件：

```c
mask |= EPOLLIN | EPOLLRDNORM;
mask |= EPOLLOUT | EPOLLWRNORM;
```

- `EPOLLIN`：当前有数据可读。
- `EPOLLRDNORM`：有普通数据可读。
- `EPOLLOUT`：当前有空间可写。
- `EPOLLWRNORM`：可以写入普通数据。
- `|` 合并多个事件位，`|=` 把新事件加入原有掩码而不清除已有事件。

`poll_wait(file, queue, wait)` 不会在调用位置立即睡眠。它把驱动的等待队列登记给 poll 框架，使 poll 框架在没有事件就绪时知道应该睡在哪些队列上。驱动随后在 mutex 保护下检查 FIFO：非空报告可读，未满报告可写。

读写成功后使用：

```c
wake_up_interruptible_poll(..., event_mask);
```

它通知 poll 等待者“某类状态可能已经改变”。进程被唤醒后，poll 框架会再次调用驱动的 `.poll` 回调重新检查真实状态；唤醒本身不等于事件一定仍然成立。

## 4. 阻塞、非阻塞、信号和 errno

- 默认阻塞读取：FIFO 为空时，`read()` 不能立即完成，进程通过 `wait_event_interruptible()` 进入可中断睡眠，直到数据到来或收到可捕获信号。
- `O_NONBLOCK`：用户明确要求不要等待。FIFO 为空时读、FIFO 已满时写会立即失败，用户态得到 `-1`，`errno == EAGAIN`。
- 信号中断：进程阻塞在 `read()` 时收到未设置自动重启的 `SIGUSR1`，内核等待函数返回中断状态，系统调用边界把它转换为用户态的 `read() == -1` 与 `errno == EINTR`。
- `SIGUSR1` 在测试中由父进程调用 `kill(child_pid, SIGUSR1)` 发送；它不是驱动自动生成的信号。
- `errno` 是用户态线程保存的最近一次失败原因。只有系统调用或库函数已经报告失败时才读取它，成功返回时不能依靠旧的 `errno` 判断结果。

阻塞、非阻塞和信号中断分别解决三个问题：

```text
阻塞：条件暂时不满足，愿意等待。
EAGAIN：条件暂时不满足，但调用者要求立即返回。
EINTR：原本正在等待，但信号要求进程先处理其他事件。
```

## 5. mutex、等待队列与并发修正

mutex 和等待队列都属于内核同步工具，但职责不同：

- mutex 保护共享 FIFO 的内容和状态，保证同一时刻只有一个执行流修改临界区。
- wait queue 保存因条件不满足而睡眠的任务，并在条件可能改变时负责通知它们。
- 等待队列不能替代锁；mutex 也不能替代睡眠和唤醒机制。

等待前必须释放 mutex，否则另一进程无法进入读写临界区改变 FIFO 状态，等待条件永远无法成立。被唤醒后还必须重新检查条件，因为多个等待者可能同时醒来，先运行的进程可能已经消费数据或占用空间。

并发复查发现原始条件：

```c
!kfifo_is_empty(&kqueue_fifo)
!kfifo_is_full(&kqueue_fifo)
```

在 `wait_event_interruptible()` 内检查时没有持有 `kqueue_fifo_lock`。因此新增 `kqueue_fifo_has_data()` 和 `kqueue_fifo_has_space()`：函数内部短暂加锁、读取状态、解锁并返回布尔结果。等待仍在不持锁状态下发生，但每次状态快照都有 mutex 保护。

## 6. 生产者、消费者和部分写入

`write()` 返回正数时也可能只写入请求的一部分。例如请求写 127 字节，但 FIFO 当时只剩 105 字节空间，驱动可以返回 105。用户程序必须根据实际返回值移动偏移量，再继续写剩余 22 字节：

```text
第一次 write：请求 127，返回 105
第二次 write：从偏移 105 开始，请求剩余 22
最终全部送达
```

一次驱动 `write()` 在 mutex 保护下不会与另一写调用同时修改 FIFO，但一个应用层逻辑消息可能由多次 `write()` 才能完成。多个生产者的多次调用之间仍可能交错，因此当前驱动是字节流队列，不提供消息边界，也不保证整个应用层消息原子写入。

生产者/消费者测试完成两阶段验证：

- 单生产者写入 1024 个确定字节，消费者逐字节验证顺序。
- 两个生产者分别写入 6000 个 `A` 和 6000 个 `B`，消费者最终读取 12000 字节并确认两类各 6000 个。

## 7. 错误路径和模块生命周期

错误路径测试使用无效用户地址调用 `write()` 和 `read()`，确认用户态均得到 `-1/EFAULT`。失败读取后再次使用有效缓冲区读取到原先的 `X`，证明错误路径没有错误消费 FIFO 数据。

`kfifo_from_user()` 与 `kfifo_to_user()` 负责安全访问用户地址。驱动不能直接解引用用户指针；用户地址可能无效、未映射或无访问权限。

`file_operations.owner = THIS_MODULE` 让打开的设备文件持有模块引用。设备仍被进程打开时执行 `rmmod kqueue_lab` 返回 `Resource temporarily unavailable`；关闭最后一个文件引用后引用计数下降，模块可以安全卸载。这避免驱动代码已经移除后仍有进程调用其文件操作。

## 8. SIGIO 与 fasync 异步通知

poll 是用户进程主动调用并等待；异步 I/O 则由驱动在事件发生后主动发送 `SIGIO`。用户态需要完成两项设置：

```c
fcntl(fd, F_SETOWN, getpid());
fcntl(fd, F_SETFL, flags | O_ASYNC);
```

- `F_SETOWN` 指定 `SIGIO` 接收者，`getpid()` 表示当前进程。
- `F_GETFL` 先读取原有文件状态标志。
- `F_SETFL` 配合 `flags | O_ASYNC` 保留原标志并启用异步通知。

驱动使用：

```c
static struct fasync_struct *kqueue_async_queue;
```

保存异步通知订阅链表的头指针。它不保存 FIFO 数据，只保存哪些打开文件订阅了通知。静态全局指针初始为 `NULL`。

`kqueue_fasync(fd, file, on)` 调用：

```c
fasync_helper(fd, file, on, &kqueue_async_queue);
```

- `on != 0` 时加入或更新订阅。
- `on == 0` 时删除当前文件的订阅。
- 传入 `&kqueue_async_queue` 是因为辅助函数需要修改链表头指针本身。

成功写入后：

```c
kill_fasync(&kqueue_async_queue, SIGIO, POLL_IN);
```

遍历订阅者并发送 `SIGIO`，`POLL_IN` 说明事件类型是普通可读数据。文件关闭时 `kqueue_release()` 调用 `kqueue_fasync(-1, file, 0)` 注销，避免留下失效订阅。

完整通知链：

```text
F_SETOWN 指定接收进程
→ O_ASYNC 触发 VFS 调用驱动 .fasync
→ fasync_helper 登记订阅
→ 另一进程 write("Z")
→ kqueue_write 将 Z 放入 FIFO
→ kill_fasync 发送 SIGIO/POLL_IN
→ 用户信号处理函数设置 got_sigio
→ 主流程 read() 读取并验证 Z
→ close() 触发 .release 注销订阅
```

测试在等待前暂时屏蔽 `SIGIO`，再用 `sigsuspend()` 原子地解除屏蔽并睡眠，避免信号刚好在“检查标志”和“开始等待”之间到达造成竞态。信号处理函数只修改 `volatile sig_atomic_t` 标志，复杂读取留在主流程执行。

## 9. 内核主要能力分类

```text
Linux 内核
├─ 系统调用与用户/内核边界
├─ 进程管理：调度、信号、定时器、进程生命周期
├─ 内存管理：虚拟内存、页分配、映射、缓存
├─ 文件与 I/O：VFS、文件系统、页缓存、块层、poll/epoll
├─ 设备模型与驱动：字符设备、总线、设备、驱动、电源管理
├─ 网络：协议栈、socket、网络设备
├─ 进程间通信：管道、共享内存、消息队列、futex
├─ 同步原语：mutex、spinlock、semaphore、completion、wait queue
├─ 安全：权限、capability、LSM
├─ 虚拟化与架构代码
└─ 调试与可观测性：printk、tracepoint、ftrace、perf、BPF
```

`kqueue_lab` 当前主要连接了 VFS、poll、等待队列、mutex、信号、用户内存拷贝和模块生命周期。

## 10. 产生或修改的文件

- `drivers/kqueue_lab/kqueue_lab.c`
  - 增加 poll 回调、带事件掩码的唤醒、带锁状态助手和 fasync/SIGIO 支持。
- `userspace/kqueue_blocking_write_test.c`
  - 验证满 FIFO 上的阻塞写与读者腾出空间后的唤醒。
- `userspace/kqueue_signal_interrupt_test.c`
  - 验证 `SIGUSR1` 打断阻塞读后用户态得到 `EINTR`。
- `userspace/kqueue_poll_test.c`
  - 验证即时状态、超时、读就绪和写就绪通知。
- `userspace/kqueue_producer_consumer_test.c`
  - 验证单生产者顺序、双生产者数量和部分传输循环。
- `userspace/kqueue_error_test.c`
  - 验证无效用户缓冲区的 `EFAULT` 与数据保持。
- `userspace/kqueue_hold_open_test.c`
  - 验证设备打开期间不能卸载模块。
- `userspace/kqueue_sigio_test.c`
  - 验证 fasync 登记、`SIGIO` 通知与读取结果。

## 11. QEMU ARM64 实际验证结果

- `kqueue_test`：退出码 0。零长度读写、`abc` 往返、空 FIFO 非阻塞 `EAGAIN`、超过容量请求的部分传输均通过。
- `kqueue_blocking_test`：退出码 0。空 FIFO 上读者保持阻塞，写入 `abc` 后被唤醒并完整读出。
- `kqueue_blocking_write_test`：退出码 0。满 FIFO 上写者保持阻塞，读走 3 字节后写入 `XYZ`，最终顺序正确。
- `kqueue_signal_interrupt_test`：退出码 0。阻塞读取收到 `SIGUSR1` 后返回 `-1/EINTR`。
- `kqueue_poll_test`：退出码 0。空、非空未满、满、重新有空间和重新为空的状态掩码正确；200 ms 超时、`POLLIN` 和 `POLLOUT` 唤醒均通过。
- 并发修正后完整回归：上述五个测试均通过，模块正常卸载。
- `kqueue_producer_consumer_test`：退出码 0。1024 字节顺序校验与两个 6000 字节生产者计数均通过；运行日志实际出现部分写入。
- `kqueue_error_test`：退出码 0。无效读写缓冲区返回 `EFAULT`，失败读取未消费数据。
- `kqueue_hold_open_test`：持有设备时 `rmmod` 退出码 1；关闭后 `rmmod` 退出码 0。
- `kqueue_sigio_test`：退出码 0。父进程收到 `SIGIO` 并读取到子进程写入的 `Z`。

所有测试运行于 Linux 6.12.27 ARM64 QEMU 来宾。模块最终均正常卸载，来宾使用 `poweroff` 正常关闭。

## 12. 遇到的问题与处理

- 初次加入 fasync 时包含了不存在的 `<linux/fasync.h>`。Linux 6.12.27 中所需声明由现有 `<linux/fs.h>` 提供，移除多余 include 后模块编译成功。
- 初次部署 SIGIO 测试时在 Buildroot 子目录使用了错误的相对路径，`cp` 明确报告源文件不存在。改用 `../drivers/...` 和 `../userspace/...` 后重新复制并重建 rootfs。
- 第一次 QEMU 启动的终端会话标识未保留，虚拟机仍运行但串口无法继续输入。确认进程后结束该测试实例，重新建立可交互会话并完成测试。
- 错误路径测试初版使用编译期常量无效指针，交叉编译器正确给出越界访问警告。改为在运行时取得页大小并构造测试地址后，无警告编译并在 QEMU 中验证真实 `EFAULT`。

## 13. 当前掌握程度与下一步

当前需要达到的程度：

- 能独立解释用户态系统调用经过 VFS/poll 框架进入驱动回调的路径。
- 能读懂并修改 `.poll`、`poll_wait()`、事件掩码和就绪唤醒代码。
- 能解释 mutex 与等待队列的职责、等待前释放锁和醒后重查条件的原因。
- 能读懂 `fasync_helper()`、`kill_fasync()`、`F_SETOWN`、`O_ASYNC` 和 `SIGIO` 的完整通知链。
- 能调试 `EAGAIN`、`EINTR`、`EFAULT`、部分传输及模块引用计数问题。

下一步建议实现一个小型 `ioctl` 查询接口，让用户态读取 FIFO 当前数据量和剩余空间，继续学习命令编码、用户态结构体拷贝和 ABI 设计。
