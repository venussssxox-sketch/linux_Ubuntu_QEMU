# Day 2 总结：miscdevice 与内核 FIFO 基本读写

日期：2026-11-11

## 1. 今天具体完成了什么？

- 创建 `kqueue_lab` 外部模块并使用 `miscdevice` 注册 `/dev/kqueue_lab` 字符设备。
- 通过 `file_operations` 将用户态 `open/read/write/close` 分派到驱动回调。
- 使用内核 `kfifo` 实现容量为 4096 字节的共享字节队列。
- 使用 mutex 保护 FIFO，避免多个执行流同时修改共享状态。
- 使用 `kfifo_from_user()` 将用户数据写入内核 FIFO，使用 `kfifo_to_user()` 将 FIFO 数据复制回用户态。
- 实现第一版非阻塞语义：FIFO 为空时读取返回 `-EAGAIN`，FIFO 没有空间时写入返回 `-EAGAIN`。
- 在 QEMU ARM64 来宾中使用 Shell 命令完成 `abc` 往返、空 FIFO 和满 FIFO 验证。

## 2. 用户态到驱动的数据路径

```text
用户程序 write()
→ 系统调用进入内核
→ VFS 根据 fd 找到 /dev/kqueue_lab 的 struct file
→ file_operations.write
→ kqueue_write()
→ kfifo_from_user()
→ kqueue_fifo
```

```text
kqueue_fifo
→ kqueue_read()
→ kfifo_to_user()
→ 系统调用返回
→ 用户程序 read() 得到数据
```

`/dev/kqueue_lab` 是设备节点，不是保存数据的普通磁盘文件。真正的数据保存在驱动的内核 FIFO 中，设备节点负责让用户程序通过统一文件接口进入驱动。

## 3. 核心对象的职责

```c
static DEFINE_KFIFO(kqueue_fifo, unsigned char, 4096);
static DEFINE_MUTEX(kqueue_fifo_lock);
```

- `kqueue_fifo`：保存最多 4096 字节的共享字节流。
- `kqueue_fifo_lock`：保护 FIFO 的内容、读写位置和空满状态。
- `copied`：记录本次操作实际传输的字节数。
- `kfifo_from_user()`：安全完成用户缓冲区到 FIFO 的复制。
- `kfifo_to_user()`：安全完成 FIFO 到用户缓冲区的复制。
- `-EAGAIN`：当前条件不满足，调用者稍后可以重试。

第一版还没有等待队列，因此空读或满写不会让进程睡眠，而是直接返回错误。

## 4. QEMU ARM64 实际验证

写入并读回 `abc`：

```text
printf 'abc' > /dev/kqueue_lab
kqueue_lab: write requested=3, copied=3

dd if=/dev/kqueue_lab of=/tmp/kqueue-read.bin bs=1 count=3
od -An -tx1 /tmp/kqueue-read.bin
61 62 63

cat /tmp/kqueue-read.bin
abc
```

读完后再次读取 1 字节，命令以非零状态退出，符合空 FIFO 返回 `EAGAIN` 的预期。

满队列测试向 4096 字节 FIFO 尝试写入 5120 字节：前四个 1024 字节块写入成功，第五次因无空间返回 `EAGAIN`。随后读出并统计得到 4096 字节，证明已经接受的数据完整保留；排空后再次读取仍报告空队列。

## 5. 产生或修改的文件

- `drivers/kqueue_lab/kqueue_lab.c`：设备注册、FIFO、mutex 和基本读写回调。
- `drivers/kqueue_lab/Makefile`：外部模块构建文件。
- `logs/kqueue-lab-fifo.md`：数据路径、命令、结果和排错记录。
- `rootfs-overlay/root/kqueue_lab.ko`：部署副本，可重新生成，不纳入 Git。

## 6. 遇到的问题与处理

- 初版使用 `DECLARE_KFIFO` 却没有执行初始化，导致 `kfifo_from_user()` 失败。改用 `DEFINE_KFIFO`，在静态定义时完成初始化。
- 主机重新生成模块后，只在仍运行的旧 QEMU 中重新 `insmod`，来宾看到的仍是旧 rootfs 内容。退出 QEMU、重建 rootfs 并重新启动后才加载到新版。
- Buildroot BusyBox 的 `od` 不支持完整 GNU 选项 `-t x1c`，改用支持的 `od -An -tx1` 查看字节。

## 7. 今天理解的关键点

- `miscdevice` 简化了字符设备注册，但底层仍通过 VFS 和 `file_operations` 分派系统调用。
- `kfifo` 是内核提供的 FIFO 实现，本项目是在使用它，不是自行实现环形队列算法。
- mutex 属于内核同步原语，负责保护共享数据；它本身不负责让空队列读者等待数据。
- 驱动必须通过用户内存复制辅助函数访问 `__user` 指针，不能把用户地址当作普通内核地址直接解引用。
- 一次读写的返回值表示实际传输字节数，不应假设请求长度一定全部完成。

## 8. 当前掌握程度与下一步

需要达到“能够读懂并修改”的程度：能解释 `miscdevice`、`file_operations`、`kfifo`、mutex、用户内存复制和 `EAGAIN` 在基本读写路径中的作用，并能修改 FIFO 容量或日志后重新验证。

下一步编写 ARM64 用户态 C 测试，把依赖人工 Shell 观察的实验改成可重复、能自动判断成功失败的程序。
