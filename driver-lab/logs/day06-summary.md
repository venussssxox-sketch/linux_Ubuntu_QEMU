# Day 6 总结：ioctl 队列状态查询

日期：2026-11-18

## 1. 今天具体完成了什么？

- 定义用户态与内核共同使用的 ioctl 接口头文件 `include/kqueue_lab_ioctl.h`。
- 定义 `struct kqueue_info`，包含 FIFO 总容量、当前数据量和剩余空间。
- 使用 `_IOR()` 编码只读查询命令 `KQUEUE_IOC_GET_INFO`。
- 在驱动中实现 `.unlocked_ioctl` 回调 `kqueue_ioctl()`。
- 使用与读写路径相同的 mutex 获取一致的 FIFO 状态快照。
- 使用 `copy_to_user()` 将查询结果安全复制到用户缓冲区。
- 编写 `userspace/kqueue_ioctl_test.c`，验证空队列、写入后、重复查询和部分读取后的状态。
- 增加未知 ioctl 命令测试，验证驱动对未支持命令返回 `ENOTTY`，且错误请求不改变 FIFO 状态。
- 使用 `mmap(PROT_NONE)` 构造不可写用户地址，验证查询结果复制失败时用户态得到 `EFAULT`，来宾内核保持正常运行。
- 使用 Buildroot ARM64 交叉工具链编译模块与测试程序，并在 Linux 6.12.27 ARM64 QEMU 来宾中完成实际验证。
- 编写 `tests/run-kqueue-regression.sh` 作为来宾内统一回归入口，自动完成模块加载、测试执行、卸载、结果统计和退出码汇总。
- 将 9 个普通功能测试纳入统一入口；每项测试重新加载模块，避免前一项遗留的 FIFO 数据影响下一项。
- 为模块生命周期测试编写独立流程：后台程序保持设备打开时确认卸载被拒绝，关闭设备后确认模块可以卸载。
- 完整自动回归在 ARM64 QEMU 中通过，汇总为 `passed: 10`、`failed: 0`，脚本退出码为 0。

## 2. ioctl 调用链

```text
用户程序 ioctl(fd, KQUEUE_IOC_GET_INFO, &info)
→ 系统调用进入内核
→ VFS 根据 fd 找到 /dev/kqueue_lab 的 struct file
→ file_operations.unlocked_ioctl
→ kqueue_ioctl(file, cmd, arg)
→ 比较 cmd，识别 KQUEUE_IOC_GET_INFO
→ 在 mutex 保护下读取 FIFO 状态
→ copy_to_user() 将内核状态快照复制到用户变量 info
→ ioctl() 返回 0，用户程序读取 info 中的字段
```

`KQUEUE_IOC_GET_INFO` 本质上是一个整数命令编号的宏名称。`_IOR(KQUEUE_IOC_MAGIC, 0x01, struct kqueue_info)` 将数据方向、命令类别、类别内序号和数据类型大小编码到该整数中。宏只生成编号，不会自动查询或复制数据。

## 3. 接口约定

```c
struct kqueue_info {
    __u32 capacity;
    __u32 used;
    __u32 available;
};
```

- `capacity`：FIFO 总容量，当前为 4096 字节。
- `used`：查询时 FIFO 中保存的数据量。
- `available`：查询时 FIFO 的剩余空间。
- 一致状态应满足 `used + available == capacity`。

查询返回的是某个时刻的状态快照，不为调用进程预留空间。查询完成后，其他进程仍可立即修改 FIFO。

## 4. 并发保护和用户内存复制

驱动在同一次 mutex 临界区内读取 `kfifo_size()`、`kfifo_len()` 和 `kfifo_avail()`，避免并发读写导致三个字段来自不同状态。状态保存到内核局部变量后立即解锁，再调用 `copy_to_user()`，避免在访问用户内存期间继续占用 FIFO 锁。

`copy_to_user()` 返回未复制的字节数：返回 0 表示全部复制成功，非零时驱动向用户返回 `-EFAULT`。驱动不能直接解引用用户传入的地址。

## 5. ARM64 QEMU 实际验证结果

测试输出：

```text
unknown command: ENOTTY PASS
bad address: EFAULT PASS
empty: capacity=4096 used=0 available=4096
after write: capacity=4096 used=3 available=4093
second query: capacity=4096 used=3 available=4093
after read: capacity=4096 used=1 available=4095
kqueue ioctl test: PASS
test_exit=0
```

结果说明：

- 空 FIFO 的状态正确。
- 未知命令在用户态表现为 `ioctl() == -1` 且 `errno == ENOTTY`。
- 未知命令执行后 FIFO 仍为空，证明错误路径没有修改队列状态。
- 合法命令配合不可写用户地址时，`copy_to_user()` 失败，用户态表现为 `ioctl() == -1` 且 `errno == EFAULT`。
- 坏地址测试没有造成内核崩溃，之后的正常查询、读写和模块卸载均成功。
- 写入 `abc` 后数据量增加到 3。
- 连续第二次查询仍为 3，证明查询没有消费 FIFO 数据。
- 读取两个字节后数据量变为 1。
- 每次查询均满足容量等于已用量与剩余空间之和。
- 模块加载、测试、卸载和来宾关机均正常完成。

## 6. 自动化回归入口

`tests/run-kqueue-regression.sh` 将单项 C 测试组织成统一回归：

```text
普通测试：清理旧模块 → 加载新模块 → 运行程序 → 卸载模块 → 统计结果
生命周期：加载模块 → 后台打开设备 → 卸载应失败 → 关闭设备 → 卸载应成功
```

普通测试根据程序退出码和模块能否正常卸载共同判断结果。生命周期测试中的第一次 `rmmod` 失败属于预期行为，第二次卸载成功才表示文件引用已经释放。脚本使用 `trap` 在正常退出或收到常见终止信号时尝试清理模块。

ARM64 QEMU 完整回归结果：

```text
=== REGRESSION SUMMARY ===
passed: 10
failed: 0
regression_exit=0
```

## 7. 产生或修改的文件

- `include/kqueue_lab_ioctl.h`：用户态与驱动共享的 ioctl ABI 定义。
- `drivers/kqueue_lab/kqueue_lab.c`：增加 ioctl 回调和操作表登记。
- `userspace/kqueue_ioctl_test.c`：新增状态查询测试。
- `tests/run-kqueue-regression.sh`：新增统一功能与生命周期回归入口。
- `README.md`：补充 ioctl 已实现能力。
- `logs/day06-summary.md`：记录本日学习内容和实测结果。

## 8. 当前掌握程度与下一步

当前需要达到的程度：

- 能解释 ioctl 三个用户态参数如何对应驱动中的 `file`、`cmd` 和 `arg`。
- 能区分命令编号、数据类型、具体变量和变量地址。
- 能解释 `_IOR()` 只负责编码命令编号，实际行为由驱动回调实现。
- 能解释为什么查询状态需要 mutex，以及为什么解锁后再调用 `copy_to_user()`。
- 能根据 `ENOTTY`、`EFAULT` 和状态不一致等现象定位基本错误。
- 能解释 Shell 函数参数、命令退出码、`if/then/fi`、后台进程、`$!`、`wait` 和测试汇总逻辑。
- 能区分“命令成功”和“测试通过”：未知 ioctl、坏地址和设备打开时卸载失败都是预期错误路径。

QEMU GDB 调试通路已经配置并完成首次源码级验证。下一步进行调试会话收尾，随后复查代码和资源生命周期，执行有界重复回归与长时间稳定性测试，并保存可复现证据。

## 9. QEMU GDB 源码级调试进展

- 使用 QEMU `-s -S` 在 TCP 1234 端口提供 GDB 远程调试，并让虚拟 CPU 在启动时暂停。
- 使用带 DWARF 4 信息的 `vmlinux` 成功在 `init/main.c:905` 定位并命中 `start_kernel()`。
- 启用 Linux 内核 GDB 辅助脚本，使用 `lx-symbols` 加载运行中的 `kqueue_lab.ko` 模块符号。
- 在 `kqueue_write()` 设置 `count == 3` 条件断点，观察到用户缓冲区为 `abc`，并验证 `kfifo_from_user()` 执行后 `copied == 3`、FIFO 有效数据量为 3。
- 在 `kqueue_read()` 设置 `count == 2` 条件断点，验证 `kfifo_to_user()` 将 `ab` 复制回用户缓冲区，FIFO 的 `out` 从 0 前进到 2，有效数据量从 3 降为 1，逻辑剩余字节为 `c`。
- 使用 `bt` 观察 `__arm64_sys_write → ksys_write → vfs_write → kqueue_write` 以及对应的读取调用路径。
- 使用 `finish` 和 `print/d $x0` 验证 `kqueue_read()` 返回值 2 经 ARM64 的 `x0` 寄存器传回调用者。
- 从反汇编确认函数参数由 `x0`、`x1`、`x2` 传入，并确认 `ldr w0, [sp, #12]` 在返回前把 `copied` 装入返回值寄存器。
- 发现当前 `lx-symbols` 会话正确重定位了模块代码符号，但 `kqueue_fifo` 数据符号仍指向代码区。通过模块真实的 `MOD_DATA.base` 与 ELF 符号偏移 `0xa0`，定位到实际 FIFO 地址 `0xffff800078c820a0`；反汇编中的 `adrp` 与 `add #0xa0` 再次验证了该地址。

本阶段需要掌握：断点、条件断点、临时断点、单步、调用栈、函数参数、局部变量、寄存器返回值，以及代码区和数据区地址的区别。手动修正模块数据符号地址属于进阶调试技巧，当前以理解定位逻辑为主。
