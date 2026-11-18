# ARM64 Linux 数据队列字符设备驱动实验

本项目在 Ubuntu x86_64 主机上使用 Buildroot 和 QEMU 构建 ARM64 Linux 实验环境，并实现 `/dev/kqueue_lab` 数据队列字符设备。驱动以 `miscdevice` 注册设备，使用 4096 字节 `kfifo` 保存共享字节流，并逐步实现非阻塞 I/O、阻塞等待、并发保护、poll 就绪通知、错误路径、模块生命周期和 `SIGIO/fasync` 异步通知。

当前所有功能均已使用 Linux 6.12.27 ARM64 QEMU 来宾实际验证。每日学习过程和验证证据按时间记录在 `logs/day01-summary.md` 至 `logs/day05-summary.md`。

## 当前实现

- `read()` / `write()`：支持零长度操作、部分传输与安全的用户内存拷贝。
- `O_NONBLOCK`：空队列读取和满队列写入返回 `EAGAIN`。
- 阻塞模式：读者等待数据，写者等待空间；等待可被信号中断并在用户态表现为 `EINTR`。
- 并发保护：mutex 保护 FIFO，wait queue 负责睡眠与唤醒，等待条件通过带锁助手读取。
- `.poll`：FIFO 非空报告可读，FIFO 未满报告可写，并支持超时和状态变化唤醒。
- `.fasync`：用户设置 `F_SETOWN` 与 `O_ASYNC` 后，成功写入触发 `SIGIO/POLL_IN`。
- 模块生命周期：打开的设备文件持有模块引用，关闭后才能安全卸载。
- 用户态测试：覆盖基本读写、阻塞读写、信号中断、poll、生产者/消费者、`EFAULT`、模块引用和 `SIGIO`。

## 主要目录

```text
driver-lab/
├── drivers/kqueue_lab/       # 内核模块源码和外部模块 Makefile
├── userspace/                # ARM64 用户态测试源码
├── env-check/                # 最小环境验收模块源码
├── logs/                     # 每日总结、实验说明和实际串口日志
├── rootfs-overlay/           # QEMU 来宾部署目录（生成的二进制不纳入 Git）
├── buildroot-config.initial  # Buildroot 配置备份
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

将模块和所需测试程序复制到 `rootfs-overlay/root/`，再进入 Buildroot 目录执行 `make` 重建 `rootfs.ext4`。启动 QEMU 后加载模块并运行测试：

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
rmmod kqueue_lab
poweroff
```

各程序的测试目标、实际退出码和关键输出见 `logs/day05-summary.md`。二进制文件和模块构建产物可由源码重新生成，因此不提交到 Git。

## 本次排错记录

1. Linux 源码下载：代理 TLS 中断，Buildroot 备用地址返回 404。重新从官方地址下载 linux-6.12.27.tar.xz，按 board/qemu/patches/linux/linux.hash 验证 SHA-256 后放入 dl/linux。make linux-headers-source 已验证缓存。
2. glibc 源码下载：Git 拉取因 GnuTLS 中断而重试。改为从 sources.buildroot.net/glibc 下载 Buildroot 提供的对应 git4 源码归档，按 package/glibc/glibc.hash 校验后放入 dl/glibc，保留原有成功构建产物继续构建。
3. 未禁用 TLS 证书检查、未跳过源码哈希检查、未修改所选软件版本。

模块首次加载时内核可能提示 out-of-tree module taints kernel：表示加载了树外模块，不等同于加载失败。是否成功以返回值、/proc/modules 和模块日志共同判断。

本文件中的使用步骤须以实际验收结果为准；最终结果记录在 logs/environment-verified.txt（完成后生成）。
