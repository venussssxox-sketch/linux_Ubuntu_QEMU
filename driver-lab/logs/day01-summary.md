# Day 1 总结：ARM64 QEMU 驱动环境验收

日期：2026-11-10

## 1. 今天具体完成了什么？

- 明确实验目标：在 Ubuntu x86_64 主机上构建并运行 ARM64 Linux，在其中加载自己编译的外部内核模块。
- 使用 Buildroot 2025.02.19 统一生成 ARM64 交叉工具链、Linux 6.12.27 内核和根文件系统。
- 使用 QEMU `virt` 平台启动 ARM64 来宾系统，并通过串口进入 Buildroot Shell。
- 编写最小 `envcheck` 外部模块，验证模块初始化函数和退出函数可以实际执行。
- 配置 rootfs overlay，将模块部署到来宾 `/root/`。
- 编写 `verify-qemu.py` 自动执行架构、模块加载、模块存在、内核日志、模块卸载和关机检查。
- 保存完整 QEMU 串口记录与简明验收结论。

## 2. 环境组成及关系

```text
Ubuntu x86_64 主机
├─ 编辑源码和保存日志
├─ 运行 Buildroot
├─ 使用 ARM64 交叉编译器生成 ARM64 程序与模块
└─ 运行 QEMU
       └─ 模拟 ARM64 virt 机器
              └─ 启动 Linux 6.12.27
                     └─ 挂载 Buildroot rootfs
                            └─ 加载 envcheck.ko
```

QEMU 负责模拟目标硬件，交叉编译器负责生成 ARM64 机器代码，Linux 内核负责在来宾中加载和执行模块。主机是 x86_64 不妨碍生成并运行 ARM64 模块，因为编译目标和运行目标都明确指定为 ARM64。

## 3. 关键验证及实际结果

自动验收记录给出的结果为：

```text
environment verification: PASS
Buildroot overlay configured: PASS
QEMU ARM64 boot: PASS
external module load/unload: PASS
module log initialization/cleanup: PASS
temporary snapshot VM shutdown: PASS
```

来宾中的关键证据包括：

- `uname -a` 输出包含 `aarch64`，证明进入的是 ARM64 Linux。
- `insmod /root/envcheck.ko` 成功，内核打印 `ARM64 driver environment loaded successfully`。
- `/proc/modules` 中存在 `envcheck`，证明模块处于已加载状态。
- `rmmod envcheck` 成功，内核打印 `module unloaded successfully`。
- 卸载后 `/proc/modules` 中不再存在模块。
- `poweroff` 正常结束临时 QEMU 实例。

内核首次加载树外模块时打印 `out-of-tree module taints kernel`，它表示模块不属于该内核源码树，不等于加载失败。

## 4. 产生的文件

- `buildroot-config.initial`：Buildroot 配置备份。
- `env-check/envcheck.c`：最小模块源码。
- `env-check/Makefile`：外部模块构建入口。
- `build-envcheck.sh`：使用匹配内核目录和交叉工具链构建模块。
- `start-qemu.sh`：启动 ARM64 QEMU 来宾。
- `verify-qemu.py`：自动化环境验收。
- `logs/environment-verified.txt`：简明 PASS 结果。
- `logs/qemu-envcheck.log`：实际串口记录。

生成的 `.ko`、`.o`、rootfs 和 Buildroot 输出可以由源码与配置重新生成，因此不作为 Git 源文件提交。

## 5. 遇到的问题与处理

- Linux 源码下载遇到代理 TLS 中断和备用地址 404。改从官方来源取得指定版本，按 Buildroot 哈希文件校验后放入下载缓存。
- glibc Git 拉取遇到 GnuTLS 中断。改用 Buildroot 源码镜像中的对应归档，并保留哈希校验。
- 没有通过关闭 TLS 验证或跳过哈希检查来绕过问题，保证构建输入仍可验证。

## 6. 今天理解的关键点

- 内核模块必须使用与运行内核匹配的构建目录和配置。
- 用户态 glibc 不参与内核模块编译；模块使用的是内核接口。
- rootfs overlay 是把主机文件部署进来宾镜像的入口，更新 overlay 后需要重新构建 rootfs。
- QEMU 的 `-snapshot` 让来宾磁盘修改在关机后丢弃，因此源码和实验记录必须保存在主机工作区。
- 判断模块是否成功不能只看一条日志，应结合命令返回值、`/proc/modules` 和初始化/退出日志。

## 7. 当前掌握程度与下一步

需要达到“能够解释并重新执行”的程度：能说明主机、交叉编译器、Buildroot、QEMU、ARM64 内核和 rootfs 的关系，能够重新构建模块、启动来宾并验证加载与卸载。

下一步是在已验收环境中实现真正的数据队列字符设备，而不再停留在只打印日志的最小模块。
