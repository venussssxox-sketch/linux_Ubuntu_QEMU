# Day 4 总结：阻塞与非阻塞 I/O

日期：2026-11-16

## 1. 今天具体完成了什么？

- 回顾了 Linux 文件 I/O、ARM64 QEMU 驱动环境、`miscdevice`、`kfifo` 和用户态测试的已有成果。
- 理解了阻塞与非阻塞 I/O 的区别：默认阻塞操作在条件不满足时进入等待队列，设置 `O_NONBLOCK` 的操作则立即返回 `-EAGAIN`。
- 理解了 `-EAGAIN`、`-ERESTARTSYS`、用户态 `EINTR`、正常等待队列唤醒和 Unix 信号中断之间的区别。
- 在 `kqueue_lab` 中加入读等待队列和写等待队列。
- 将 `kqueue_read()` 改为支持阻塞读取和 `O_NONBLOCK`：FIFO 为空时阻塞读者进入等待队列，FIFO 有数据时在 mutex 保护下读取。
- 将 `kqueue_write()` 改为支持阻塞写入和 `O_NONBLOCK`：FIFO 已满时阻塞写者进入等待队列，FIFO 有空间时在 mutex 保护下写入。
- 实现读写双方的交叉唤醒：成功写入后唤醒读者，成功读取后唤醒写者。
- 将原有用户态回归测试改为使用 `O_NONBLOCK` 打开设备，保留空 FIFO 返回 `EAGAIN` 的验证语义。
- 编写了阻塞读取测试程序的主体，使用 `fork()` 创建父子进程，并使用普通管道同步“子进程即将读取”的时机。
- 学习了 `pipe()`、`fork()`、`waitpid(..., WNOHANG)`、`alarm()` 和父子进程文件描述符继承在并发测试中的作用。
- 补全阻塞测试的 `fork()` 失败退出路径，并使用 ARM64 交叉工具链完成编译检查。

## 2. 实现的数据流

阻塞读取：

```text
子进程 read()
→ kqueue_read()
→ FIFO 为空
→ 释放 mutex
→ 在 kqueue_read_queue 中进入可中断睡眠
→ 父进程 write("abc")
→ kqueue_write() 写入 FIFO
→ wake_up_interruptible(&kqueue_read_queue)
→ 子进程重新获得 mutex 并检查 FIFO
→ 读出 abc
```

阻塞写入：

```text
写进程 write()
→ kqueue_write()
→ FIFO 已满
→ 释放 mutex
→ 在 kqueue_write_queue 中进入可中断睡眠
→ 读进程读走数据
→ wake_up_interruptible(&kqueue_write_queue)
→ 写进程重新获得 mutex 并检查 FIFO
→ 继续写入
```

## 3. 产生或修改了哪些文件？

- `drivers/kqueue_lab/kqueue_lab.c`
  - 新增 `linux/wait.h`。
  - 新增 `kqueue_read_queue` 和 `kqueue_write_queue`。
  - 为 `read/write` 增加阻塞等待、非阻塞返回、信号中断处理和交叉唤醒。
- `userspace/kqueue_test.c`
  - 使用 `O_RDWR | O_NONBLOCK` 打开设备，继续验证零长度读写、数据往返、部分传输和 `EAGAIN`。
- `userspace/kqueue_blocking_test.c`
  - 编写父子进程阻塞读取测试主体。
- `drivers/kqueue_lab/kqueue_lab.ko`
  - 重新生成的 ARM64 内核模块构建产物，未计划纳入 Git。
- `userspace/build/kqueue_test`
  - 重新生成的 ARM64 用户态测试构建产物，未计划纳入 Git。
- `userspace/build/kqueue_blocking_test`
  - 新生成的 ARM64 阻塞测试构建产物，未计划纳入 Git。

## 4. 已完成的验证

### 驱动模块编译

使用 Linux 6.12.27 构建目录和 Buildroot ARM64 交叉工具链完成外部模块编译。构建阶段包括：

```text
CC [M]  kqueue_lab.o
MODPOST Module.symvers
LD [M]  kqueue_lab.ko
```

结果：编译成功，无 `error:`；生成的 `kqueue_lab.ko` 被识别为 ARM aarch64 ELF 可重定位模块。

### 非阻塞用户态测试编译

使用 `aarch64-buildroot-linux-gnu-gcc` 和 `-Wall -Wextra -Wpedantic -O2` 重新编译 `kqueue_test.c`。

结果：编译成功，无警告；生成的程序被识别为 ARM aarch64 ELF 可执行文件。

### 阻塞用户态测试编译

使用相同的 ARM64 交叉工具链和 `-Wall -Wextra -Wpedantic -O2` 编译 `kqueue_blocking_test.c`。

结果：编译成功，无警告；生成的程序被识别为 ARM aarch64 ELF 可执行文件。该结果只证明源码和用户态接口通过编译，尚未证明 QEMU 中的阻塞与唤醒运行行为。

## 5. 今天理解的关键点

- `for (;;)` 本身不负责睡眠；它负责在进程被正常唤醒后重新获得 mutex 并重新检查 FIFO。
- 真正使进程睡眠的是 `wait_event_interruptible()`。
- `wake_up_interruptible()` 只使等待进程重新具备运行资格，实际运行时间由调度器决定。
- 等待前必须释放 mutex，否则写者或读者无法改变等待条件，会形成死锁。
- 正常等到条件成立时，`wait_event_interruptible()` 返回 `0`；等待被 Unix 信号打断时返回负错误码。
- `ret` 保存辅助函数的成功或错误状态，`copied` 或用户态的 `transferred` 保存实际传输字节数。
- `read()` 是统一系统调用入口；读取普通管道时进入内核管道实现，读取 `/dev/kqueue_lab` 时进入驱动的 `kqueue_read()`。
- `read()` 只保证前 `transferred` 个缓冲区字节来自本次读取。必须先验证长度，再对完整期望长度执行 `memcmp()`，否则可能比较旧值并出现假通过。

## 6. 尚未完成的验证

- 将新版模块和两个测试程序放入 rootfs overlay，并重建 Buildroot 根文件系统。
- 在 QEMU ARM64 来宾中运行非阻塞回归测试。
- 在 QEMU ARM64 来宾中运行阻塞读取测试，确认读者先睡眠、写者写入后读者被唤醒。
- 单独验证 FIFO 已满时的阻塞写入和读者腾出空间后的写者唤醒。
- 验证阻塞等待被信号打断时的用户态行为。

## 7. 遇到的问题与处理

- 驱动加入阻塞语义后，原有测试仍以 `O_RDWR` 打开设备并期待空 FIFO 返回 `EAGAIN`，会导致测试在空队列读取处正常阻塞。已将原有回归测试改为 `O_RDWR | O_NONBLOCK`，把非阻塞测试与阻塞测试分开。
- 阻塞测试不能只让父进程立即写入，否则子进程可能尚未进入 `read()`。测试使用普通管道传递准备通知，并用 `waitpid(..., WNOHANG)`检查子进程没有提前退出。
- 仅执行 `memcmp(buffer, message, 3)` 不能证明本次读取获得了 3 字节；缓冲区未被本次 `read()` 写入的部分可能包含旧值。测试应先检查 `transferred == 3`，再比较内容。
- 阻塞测试的初版 `fork()` 失败分支关闭了资源但缺少 `return 1`，会继续使用无效的 `pid == -1` 执行父进程路径。提交前已补充错误返回，并通过交叉编译检查。

## 8. 对应的岗位能力

- Linux 字符设备驱动开发。
- 等待队列、进程状态和内核调度基础。
- 阻塞与非阻塞 I/O 语义设计。
- mutex、共享 FIFO 和并发条件检查。
- C 语言系统调用、进程创建和进程间同步。
- ARM64 交叉编译和 QEMU 驱动验证。
- 正常路径、错误路径、部分传输和并发行为测试。

## 9. 下一次开始位置

1. 部署新版模块和两个测试程序到 rootfs overlay。
2. 重建 rootfs 并在全新 QEMU 来宾中依次运行非阻塞和阻塞测试。
3. 保存实际终端输出并补充运行结论。
4. 编写并验证 FIFO 已满时的阻塞写测试。
