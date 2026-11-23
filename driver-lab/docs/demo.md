# 3 分钟演示流程

## 0:00—0:30：项目目标

介绍：在 x86_64 Ubuntu 主机上用 Buildroot 构建 ARM64 Linux 6.12.27，并在 QEMU `virt` 来宾中运行自编译字符设备模块。设备使用 miscdevice 注册 `/dev/kqueue_lab`，用 kfifo 保存共享字节流。

展示 README 的目录与接口概览，不播放完整构建过程。

## 0:30—1:20：核心数据路径

展示 `drivers/kqueue_lab/kqueue_lab.c`：

```text
用户 read/write
→ VFS file_operations
→ mutex 保护 kfifo
→ 条件不满足时释放锁并进入等待队列
→ 对端改变状态后唤醒
→ 重新加锁检查并传输
```

说明非阻塞返回 `EAGAIN`，阻塞等待可被信号打断；poll 查询就绪，SIGIO 主动通知，ioctl 查询容量、已用量和可用量。

## 1:20—2:20：运行证据

从已有镜像执行：

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
python3 tests/repeat-qemu-regression.py \
  --rounds 1 --expected-tests 13 --timeout 120 --label demo
```

预期看到 `round 1/1: PASS`。说明每轮包含基本读写、阻塞读写、信号中断、poll、生产者/消费者、错误地址、部分用户拷贝、部分拷贝唤醒、多读者与反复打开关闭、SIGIO、ioctl 和模块生命周期。

## 2:20—2:50：真实问题

说明 Linux kfifo 在用户内存只复制部分数据时会推进 FIFO 指针并返回 `EFAULT`。旧驱动直接返回错误，造成“数据已传输但用户看到完全失败”，并遗漏唤醒。修复规则是：`copied > 0` 时返回实际字节数并执行通知；只有 `copied == 0` 时返回错误。

展示 `userspace/kqueue_partial_fault_test.c` 与修复前后日志。

## 2:50—3:00：边界

明确：这是 QEMU ARM64 功能验证，不是真实硬件性能测试；约 8 小时稳定性测试尚待完成。项目的价值是驱动接口、并发语义、错误路径、自动化回归和可复现构建。
