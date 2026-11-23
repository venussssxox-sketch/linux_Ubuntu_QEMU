# ARM64 Linux 数据队列字符设备驱动实验

本项目在 Ubuntu x86_64 主机上使用 Buildroot 和 QEMU 构建 ARM64 Linux 实验环境，并实现 `/dev/kqueue_lab` 数据队列字符设备。驱动以 `miscdevice` 注册设备，使用 4096 字节 `kfifo` 保存共享字节流，并逐步实现非阻塞 I/O、阻塞等待、并发保护、poll 就绪通知、错误路径、模块生命周期、`SIGIO/fasync` 异步通知和 ioctl 状态查询。

当前所有功能均已使用 Linux 6.12.27 ARM64 QEMU 来宾实际验证。每日学习过程和验证证据按时间记录在 `logs/day01-summary.md` 至 `logs/day08-summary.md`。第 8 天完成锁与唤醒路径复查、部分用户拷贝故障修复及 10 轮有界重复回归；后续补充了部分拷贝唤醒、多读者和反复打开关闭专项测试。约 8 小时稳定性测试尚未执行。

## 当前实现

- `read()` / `write()`：支持零长度操作、部分传输与安全的用户内存拷贝。
- `O_NONBLOCK`：空队列读取和满队列写入返回 `EAGAIN`。
- 阻塞模式：读者等待数据，写者等待空间；等待可被信号中断并在用户态表现为 `EINTR`。
- 并发保护：mutex 保护 FIFO，wait queue 负责睡眠与唤醒，等待条件通过带锁助手读取。
- `.poll`：FIFO 非空报告可读，FIFO 未满报告可写，并支持超时和状态变化唤醒。
- `.fasync`：用户设置 `F_SETOWN` 与 `O_ASYNC` 后，成功写入触发 `SIGIO/POLL_IN`。
- `.unlocked_ioctl`：`KQUEUE_IOC_GET_INFO` 返回 FIFO 总容量、当前数据量和剩余空间，不消费队列数据。
- 模块生命周期：打开的设备文件持有模块引用，关闭后才能安全卸载。
- 用户态测试：覆盖基本读写、阻塞读写、信号中断、poll、生产者/消费者、多个读者、反复打开关闭、`EFAULT`、部分用户拷贝及其唤醒、模块引用、`SIGIO` 和 ioctl 状态查询。
- 自动化回归：来宾中的 `run-kqueue-regression.sh` 为每项普通测试重新加载模块，并单独验证设备打开期间拒绝卸载、关闭后允许卸载的生命周期行为。

## 主要目录

```text
driver-lab/
├── drivers/kqueue_lab/       # 内核模块源码和外部模块 Makefile
├── userspace/                # ARM64 用户态测试源码
├── tests/                    # 测试编排脚本
├── env-check/                # 最小环境验收模块源码
├── logs/                     # 每日总结、实验说明和实际串口日志
├── rootfs-overlay/           # QEMU 来宾部署目录（生成的二进制不纳入 Git）
├── configs/                  # 内核调试配置与可移植 Buildroot defconfig
├── build-envcheck.sh         # 环境模块构建脚本
├── start-qemu.sh             # ARM64 QEMU 启动脚本
└── verify-qemu.py            # 环境自动验收脚本
```

## 各组件的意义

| 组件 | 用途 |
| --- | --- |
| 主机 Ubuntu | 编辑源码、运行构建工具、保存文件 |
| Buildroot 2025.02.19 | 统一构建工具链、Linux 和根文件系统 |
| ARM64 交叉编译器 | 在 x86_64 主机上生成 ARM64 代码 |
| Linux 6.12.27 | 在虚拟板上管理资源和运行内核模块 |
| rootfs.ext4 | 提供用户态 init、Shell、BusyBox 和模块测试文件 |
| QEMU 6.2 的 virt 平台 | 模拟 ARM CPU 与通用虚拟硬件 |

glibc 是目标用户态的 C 标准库，不是内核模块使用的库。用于编译用户程序的内核头文件，不等于已编译的完整 Linux 内核；外部模块要使用与运行内核匹配的构建目录。

## 构建与使用

首先使用所记录的 Buildroot 配置完成构建。具体配置备份和验证日志以此目录下实际文件为准。

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab/buildroot-2025.02.19
set -o pipefail
make 2>&1 | tee -a build.log
```

意义：让 Buildroot 补齐未完成的构建步骤。成功标志是命令正常结束，并生成 output/images/Image 和 output/images/rootfs.ext4。

生成或更新环境测试模块：

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
./build-envcheck.sh
cd buildroot-2025.02.19
make
```

意义：使用同一套工具链和内核构建目录编译 envcheck.ko，并通过已配置的 rootfs-overlay 放进目标系统。修改模块后，需要重新构建根文件系统，虚拟机内才会看到新版。

启动虚拟板：

```bash
/home/zhaojinxin/linux_Ubuntu/driver-lab/start-qemu.sh
```

出现 login 时输入 root；实验镜像使用本地控制台空密码。脚本未配置从外部网络访问来宾的端口转发。

进入来宾后依次执行：

```sh
uname -m
uname -r
insmod /root/envcheck.ko
lsmod
dmesg | grep envcheck
rmmod envcheck
dmesg | grep envcheck
poweroff
```

意义和成功标志：

1. uname -m 返回 aarch64，证明进入的是 ARM64 来宾。
2. uname -r 显示所构建的内核版本。
3. insmod 成功且 lsmod 中出现 envcheck，证明外部模块与运行内核匹配。
4. dmesg 出现加载消息，证明模块初始化函数实际执行。
5. rmmod 成功且日志出现卸载消息，证明清理函数实际执行。
6. poweroff 正常关闭虚拟板并返回主机终端。

脚本默认使用 -snapshot：来宾内的磁盘写入在退出后丢弃，便于重复实验。源代码、测试结果请保存在主机工作区，不要把来宾内部当作唯一存储位置。

自动验收：

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
python3 verify-qemu.py
```

脚本需要主机 Python 的 pexpect，本机已检测到。成功会打印 PASS，串口记录保存到 logs/qemu-envcheck.log。它启动自己的临时虚拟机并在验收后关闭。

## QEMU/GDB 源码调试

调试启动脚本在普通 QEMU 参数之外增加 `-s -S`：`-s` 在 TCP 1234 端口开启 GDB 远程服务，`-S` 让虚拟 CPU 在启动时暂停。先在一个终端运行：

本次验证使用的 ARM64 内核配置保存在 `configs/linux-aarch64-virt-debug.config`，其中启用了 `CONFIG_DEBUG_KERNEL`、DWARF 4 调试信息和 `CONFIG_GDB_SCRIPTS`。

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
./start-qemu-debug.sh
```

再在另一个终端载入包含 DWARF 信息的 `vmlinux` 并连接 QEMU：

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
gdb-multiarch buildroot-2025.02.19/output/build/linux-6.12.27/vmlinux
```

```gdb
target remote :1234
hbreak start_kernel
continue
```

来宾启动并加载 `kqueue_lab.ko` 后，可以用 Linux 内核 GDB 辅助命令载入模块符号：

```gdb
lx-symbols /home/zhaojinxin/linux_Ubuntu/driver-lab/drivers/kqueue_lab
break kqueue_write if count == 3
continue
```

首次源码级调试的断点、调用栈、FIFO 状态和 ARM64 寄存器验证记录见 `logs/day06-summary.md`。

## 构建 kqueue_lab 模块

在项目目录执行：

```bash
make -C buildroot-2025.02.19/output/build/linux-6.12.27 \
  ARCH=arm64 \
  CROSS_COMPILE=/home/zhaojinxin/linux_Ubuntu/driver-lab/buildroot-2025.02.19/output/host/bin/aarch64-buildroot-linux-gnu- \
  M=/home/zhaojinxin/linux_Ubuntu/driver-lab/drivers/kqueue_lab \
  modules
```

该命令使用 QEMU 来宾正在运行的内核构建目录和匹配的 ARM64 交叉工具链，生成 `drivers/kqueue_lab/kqueue_lab.ko`。

用户态测试使用同一 Buildroot 工具链编译。例如：

```bash
mkdir -p userspace/build
buildroot-2025.02.19/output/host/bin/aarch64-buildroot-linux-gnu-gcc \
  -Wall -Wextra -Wpedantic -O2 \
  userspace/kqueue_poll_test.c \
  -o userspace/build/kqueue_poll_test
```

将模块、所需测试程序和 `tests/run-kqueue-regression.sh` 复制到 `rootfs-overlay/root/`，再进入 Buildroot 目录执行 `make` 重建 `rootfs.ext4`。启动 QEMU 后可以运行统一回归入口：

```sh
/root/run-kqueue-regression.sh
printf 'regression_exit=%s\n' "$?"
```

脚本当前依次运行 12 个普通功能测试（包括部分用户拷贝故障、部分拷贝唤醒、多个读者和反复打开关闭），加上独立模块生命周期测试，共 13 项。每项普通测试前重新加载模块、结束后卸载模块，保证 FIFO 状态相互隔离；模块生命周期测试在后台保持设备打开，确认第一次卸载被拒绝，关闭设备后第二次卸载成功。

也可以单独运行各程序定位失败：

```sh
insmod /root/kqueue_lab.ko
/root/kqueue_test
/root/kqueue_blocking_test
/root/kqueue_blocking_write_test
/root/kqueue_signal_interrupt_test
/root/kqueue_poll_test
/root/kqueue_producer_consumer_test
/root/kqueue_error_test
/root/kqueue_hold_open_test
/root/kqueue_sigio_test
/root/kqueue_ioctl_test
rmmod kqueue_lab
poweroff
```

各程序的测试目标、实际退出码和关键输出见 `logs/day05-summary.md` 与 `logs/day06-summary.md`。二进制文件和模块构建产物可由源码重新生成，因此不提交到 Git。

## 本次排错记录

1. Linux 源码下载：代理 TLS 中断，Buildroot 备用地址返回 404。重新从官方地址下载 linux-6.12.27.tar.xz，按 board/qemu/patches/linux/linux.hash 验证 SHA-256 后放入 dl/linux。make linux-headers-source 已验证缓存。
2. glibc 源码下载：Git 拉取因 GnuTLS 中断而重试。改为从 sources.buildroot.net/glibc 下载 Buildroot 提供的对应 git4 源码归档，按 package/glibc/glibc.hash 校验后放入 dl/glibc，保留原有成功构建产物继续构建。
3. 未禁用 TLS 证书检查、未跳过源码哈希检查、未修改所选软件版本。

模块首次加载时内核可能提示 out-of-tree module taints kernel：表示加载了树外模块，不等同于加载失败。是否成功以返回值、/proc/modules 和模块日志共同判断。

本文件中的使用步骤须以实际验收结果为准；最终结果记录在 logs/environment-verified.txt（完成后生成）。

## 有界重复回归与部分拷贝故障验证

主机中执行以下命令，重建当前源码、部署模块和全部测试、更新根文件系统：

```bash
bash /home/zhaojinxin/linux_Ubuntu/driver-lab/tests/build-kqueue.sh
python3 /home/zhaojinxin/linux_Ubuntu/driver-lab/tests/repeat-qemu-regression.py --rounds 10 --expected-tests 13 --timeout 120 --label regression
```

Python 编排依赖主机 pexpect；`--rounds` 是完整回归轮数，`--duration-hours` 用于按真实时长完成的完整轮次，`--timeout` 是每轮最长等待秒数，`--label` 是日志目录标签。脚本启动自己的快照虚拟机，核对镜像与 overlay 中模块及测试文件的 SHA-256，保存串口、每轮输出、检查点和 JSON 汇总，检查模块卸载、最终内核日志和正常关机。失败或超时返回非零并终止自己的虚拟机。结果存于 `logs/<UTC时间>-<标签>/`，同名目录不会覆盖。

2026-11-23 实测：修复前原有 10 项通过、新增部分故障项失败；修复后 10 轮 × 11 项全部通过，完整会话 80.275 秒。之后补充的 13 项专项回归也通过。部分用户拷贝只要已经传输了字节，就返回实际字节数并通知等待者；完全未传输才返回拷贝错误。

证据及边界见 `logs/day08-summary.md`、`logs/day08-code-review.md`、`docs/interface.md` 和 `docs/known-limitations.md`。这是 QEMU 功能重复回归，不是 8 小时稳定性测试，也不是实物性能结论。

## 从空输出目录复现

以下入口会校验固定的 Buildroot 2025.02.19 源码归档，在新的工作区解压源码、生成新的 Buildroot 输出目录、重新构建工具链/内核/rootfs，并用新镜像执行一次 13 项回归。可传入已有且经过 Buildroot 哈希校验的下载缓存以避免重复下载；它不会复用任何编译产物。

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
bash tests/clean-reproduce.sh \
  --workspace /home/zhaojinxin/driver-lab-clean-repro \
  --download-cache /home/zhaojinxin/linux_Ubuntu/driver-lab/buildroot-2025.02.19/dl
```

`--workspace` 必须是不存在的目录，避免把历史构建输出混入证据。构建日志、生成的最小配置、哈希和结果摘要保存在该工作区；回归证据仍由项目回归器写入 `logs/`。

## 8 小时稳定性测试

工具已经支持检查点和实际时长，但本仓库尚未执行该测试。准备开始时运行：

```bash
cd /home/zhaojinxin/linux_Ubuntu/driver-lab
python3 tests/repeat-qemu-regression.py \
  --duration-hours 8 --expected-tests 13 --timeout 120 \
  --label stability-8h
```

每轮结束会原子更新结果目录中的 `checkpoint.json`；正常完成后才会写 `summary.json` 的 `success: true`。完成 8 小时前不得把项目表述为已完成长期稳定性验证。
