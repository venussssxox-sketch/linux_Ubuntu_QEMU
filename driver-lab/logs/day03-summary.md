# Day 3 总结：用户态 C 回归测试与边界语义

日期：2026-11-12

## 1. 今天具体完成了什么？

- 编写 `userspace/kqueue_test.c`，使用用户态 C 程序自动验证 `/dev/kqueue_lab`。
- 使用 Buildroot 提供的 ARM64 交叉编译器生成 AArch64 ELF 测试程序。
- 将测试程序放入 rootfs overlay，重建根文件系统并在 QEMU ARM64 来宾中运行。
- 验证写入 `abc` 后可以读取相同的 3 字节并通过 `memcmp()` 比较。
- 验证数据被消费后，非阻塞读取返回 `-1` 且 `errno == EAGAIN`。
- 新增零长度读取测试，发现驱动错误返回 `EAGAIN`。
- 修复 `kqueue_read()` 与 `kqueue_write()`：`count == 0` 时直接返回 0，不访问或改变 FIFO。
- 增加零长度写入测试，复测零长度读写、普通数据往返和空队列行为全部通过。

## 2. 为什么要从 Shell 测试升级为 C 测试？

Shell 命令适合快速观察，但难以精确区分系统调用返回值和 `errno`。C 测试可以直接断言：

```text
write() 实际返回多少字节
read() 实际返回多少字节
数据内容是否逐字节一致
失败时 errno 是否正好是 EAGAIN
程序最终退出码是否为 0
```

测试程序的退出码构成自动化结果：所有条件通过时返回 0，任意断言失败时打印原因并返回 1。

## 3. 基本测试逻辑

```text
open("/dev/kqueue_lab", O_RDWR | O_NONBLOCK)
→ write(fd, "abc", 3)
→ 检查返回值为 3
→ read(fd, buffer, 3)
→ 检查返回值为 3
→ memcmp(buffer, "abc", 3)
→ 再次 read()
→ 检查返回 -1 且 errno == EAGAIN
→ close(fd)
→ 返回 0
```

`O_NONBLOCK` 是这项测试的重要条件：空 FIFO 时立即返回，测试程序不会因为默认阻塞语义而停住。

## 4. 零长度缺陷、原因与修复

初次加入：

```c
read(fd, buffer, 0);
```

实际得到：

```text
expected=0 actual=-1 errno=Resource temporarily unavailable
```

原因是驱动没有先区分零长度请求，而是继续检查空 FIFO，把“请求传输 0 字节”错误解释成“想读取数据但当前无数据”。

修复是在读写回调开头增加：

```c
if (count == 0)
    return 0;
```

零长度操作不应消费数据、增加数据或改变 FIFO 状态。修复后，零长度写和零长度读都返回 0，随后普通 `abc` 往返仍成功，最后空 FIFO 读取仍正确返回 `EAGAIN`。

## 5. QEMU ARM64 实际验证

最终输出包括：

```text
PASS: zero-length write returned 0
PASS: zero-length read returned 0
PASS: wrote and read back 3 bytes: abc
PASS: empty queue returned EAGAIN
```

测试程序退出码为 0。驱动日志确认普通写入和读取都实际传输 3 字节；零长度操作不会产生普通数据传输日志，也不会影响后续内容。

## 6. 产生或修改的文件

- `userspace/kqueue_test.c`：用户态自动回归测试。
- `logs/kqueue-userspace-test.md`：构建、部署、实际输出、缺陷与复测记录。
- `drivers/kqueue_lab/kqueue_lab.c`：增加零长度读写直接返回 0 的语义。
- `userspace/build/kqueue_test`：ARM64 构建产物，可重新生成，不纳入 Git。

## 7. 今天理解的关键点

- 系统调用返回值表达主要结果，`errno` 只在调用已经失败时解释失败原因。
- 用户态通常看到 `read() == -1` 与 `errno == EAGAIN`；驱动内部返回的是负错误码 `-EAGAIN`。
- `memcmp()` 前必须先确认实际读取长度，不能用缓冲区旧内容造成假通过。
- 测试不仅要覆盖正常数据往返，还要覆盖零长度、空队列、返回值、错误码和状态不变性。
- ARM64 交叉编译结果应使用 `file` 确认为 AArch64 ELF，不能把主机 x86_64 程序误放入来宾。
- 修改 overlay 中的程序或模块后需要重建 rootfs，并启动新的 QEMU 实例才能得到新内容。

## 8. 当前掌握程度与下一步

需要达到“能够独立编写基础测试”的程度：能使用 `open/read/write/close`，检查 `ssize_t` 返回值、`errno` 和数据内容，并用退出码表达测试结果；能为零长度和空队列设计不依赖人工判断的断言。

下一步为驱动增加等待队列，实现默认阻塞读写，并将非阻塞与阻塞行为分开测试。
