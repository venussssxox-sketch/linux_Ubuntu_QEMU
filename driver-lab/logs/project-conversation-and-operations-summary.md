# Linux 驱动前置项目：全部对话与操作整理

整理范围：本前置项目相关对话、永久日志约定、项目文件、Git 提交、QEMU/Buildroot 测试证据和最终交付状态。

说明：聊天中的提问、教学和终端操作按可核实的时间线整理；“计划”与“实际完成”分开记录。原始串口、JSON 和构建日志仍保存在 `logs/` 目录中，本文件是索引和叙述性总览。

## 一、项目目标与约定

项目名称为“基于 QEMU ARM64 的 Linux 数据队列字符设备驱动实验”。目标是在 Ubuntu x86_64 主机上使用 Buildroot 和 QEMU 构建 ARM64 Linux，编写 `/dev/kqueue_lab` 字符设备驱动，学习 Linux 文件操作、内核模块、FIFO、并发、等待队列、poll、异步通知、ioctl、调试、自动测试和可复现交付。

目标环境固定为：Buildroot 2025.02.19、Linux 6.12.27、QEMU ARM64 `virt`、Linux 6.12.27 ARM64 来宾、匹配的 AArch64 交叉工具链。

用户在永久日志中明确的长期要求：

1. 分析理解和任务拆解偏好使用 6-Astra，任务执行偏好使用 5.6-Luna；实际对话模型若无法切换，不声称已经切换。
2. 教学必须说明每一步做什么、为什么做、意义和验证方法。
3. 可以阅读用户终端输出，判断用户做到哪一步并说明下一步。
4. 输出代码时说明用户需要达到的掌握程度：理解、修改、调试或独立编写。
5. 执行终端操作前先说明命令、目的和预期；执行后说明实际结果和意义。
6. 遇到新命令，先解释命令整体作用、新选项、预期输出和结果解读。
7. 命令行编辑器优先使用 Vim，不使用 nano。
8. 实验结果和实验记录由 AI 直接写入项目文件；写入后说明改了哪些文件和内容，并提醒用户检查。
9. 实验报告记录当天实际学习内容，不写“原计划改到哪一天”等过程性归档说明。

## 二、总体计划与项目边界

原始 Linux 驱动计划把主项目分为四周：

- 第 1 周：环境、模块、miscdevice、kfifo、基本读写。
- 第 2 周：阻塞/非阻塞、边界、信号中断、首轮投递。
- 第 3 周：poll、并发、错误路径、模块生命周期、自动回归。
- 第 4 周：GDB、资源生命周期、重复回归、稳定性测试、干净复现、v1.0 交付。

计划中的扩展实验是第 5 周 I²C/SMBus i2c-stub 教学驱动；本次前置主项目收口没有开始该扩展实验。

主项目不声称已经完成真实开发板、DMA、设备树外设、真实 I²C 电气故障、真实 ARM 硬件性能或生产级权限策略。QEMU 结果只用于功能、接口、并发和可复现构建验证。

## 三、按天的学习和操作记录

### 第 1 天：ARM64 QEMU 驱动环境

完成内容：

- 明确 Ubuntu x86_64 主机、Buildroot、AArch64 交叉编译器、QEMU ARM64 virt、Linux 6.12.27 和 BusyBox rootfs 的关系。
- 构建并启动 ARM64 Linux 来宾。
- 编写并加载最小 `envcheck` 外部模块。
- 配置 rootfs overlay，把模块部署到来宾 `/root/`。
- 编写 `verify-qemu.py`，自动检查架构、模块加载、`/proc/modules`、dmesg、卸载和关机。
- 保存 `environment-verified.txt` 与 `qemu-envcheck.log`。

实际验证：ARM64 启动、模块加载、模块日志、模块卸载和临时快照关机均通过。

学习重点：QEMU 提供虚拟硬件，交叉编译器生成 ARM64 代码，Linux 来宾实际执行模块；主机是 x86_64 不妨碍交叉构建 ARM64。

### 第 2 天：miscdevice、kfifo 和基本读写

完成内容：

- 创建 `kqueue_lab` 外部模块。
- 使用 `miscdevice` 注册 `/dev/kqueue_lab`。
- 使用内核 `kfifo` 保存容量 4096 字节的共享字节流。
- 使用 mutex 保护 FIFO。
- 实现 `open/read/write/release` 基本路径。
- 使用 `kfifo_from_user()` 和 `kfifo_to_user()` 进行用户态与内核 FIFO 之间的复制。
- 初步支持空队列读、满队列写返回 `EAGAIN` 的非阻塞语义。

验证内容：来宾内写入 `abc`、读取 `abc`、检查空队列和满队列。

学习重点：设备节点不是普通磁盘文件；VFS 通过 `file_operations` 把用户系统调用分派到驱动，真正数据保存在内核 FIFO。

### 第 3 天：用户态 C 回归与边界语义

完成内容：

- 编写 `userspace/kqueue_test.c`。
- 使用 Buildroot AArch64 工具链编译用户测试程序。
- 将测试程序放入 rootfs overlay，重建 rootfs，在 ARM64 QEMU 中运行。
- 验证 `abc` 往返、数据内容、数据被消费后 `EAGAIN`。
- 增加零长度读写测试。
- 发现零长度访问错误返回 `EAGAIN`，修改驱动使 `count == 0` 直接返回 0。

学习重点：Shell 观察适合快速检查，C 测试可以精确检查返回字节数、errno、数据内容和退出码；用户程序必须处理部分读写。

### 第 4 天：阻塞与非阻塞 I/O

完成内容：

- 理解 `O_NONBLOCK`、`EAGAIN`、`-ERESTARTSYS`、用户态 `EINTR`、等待队列和信号中断。
- 增加读等待队列和写等待队列。
- 空 FIFO 的阻塞读等待数据，写入后唤醒读者。
- 满 FIFO 的阻塞写等待空间，读取后唤醒写者。
- 将既有基本测试改为明确使用 `O_NONBLOCK`。
- 编写阻塞读取测试，使用 `fork()`、`pipe()`、`waitpid()`、`WNOHANG` 和 `alarm()` 组织父子进程。
- 补齐 `fork()` 失败路径并完成 ARM64 编译检查。

教学中重点解释：

- 阻塞操作在条件不满足时睡眠；非阻塞操作立即返回 `EAGAIN`。
- 等待前必须释放 mutex，否则对端无法取得锁改变条件。
- 被唤醒后必须重新检查条件，因为另一个竞争者可能已经消费数据或空间。

### 第 5 天：poll、并发和异步通知

完成内容：

- 在 ARM64 QEMU 部署并验证阻塞读、阻塞写、非阻塞和信号中断。
- 增加 `.poll` 回调。
- 使用 `poll_wait()` 登记读/写等待队列。
- 修正条件助手读取 FIFO 状态时没有持锁的并发问题。
- 编写生产者/消费者测试。
- 验证单生产者顺序、双生产者总量和部分写入循环处理。
- 编写错误路径测试，检查非法用户地址返回 `EFAULT` 且失败读取不错误消费 FIFO。
- 编写模块生命周期测试：设备打开期间 `rmmod` 被拒绝，关闭后卸载成功。
- 增加 `.fasync`，使用 `fasync_helper()` 管理订阅，使用 `kill_fasync()` 发送 `SIGIO/POLL_IN`。
- 编写 SIGIO 测试，验证通知后读取到 `Z`。

教学中解释了用户态、系统调用、VFS、poll 通用框架和驱动回调之间的关系；也解释了 `release`、`fasync`、`ioctl` 的职责和触发时机。

### 第 6 天：ioctl、统一回归和 GDB

完成内容：

- 新增 `include/kqueue_lab_ioctl.h`。
- 定义 `struct kqueue_info`：`capacity`、`used`、`available`。
- 使用 `_IOR()` 定义 `KQUEUE_IOC_GET_INFO`。
- 实现 `.unlocked_ioctl = kqueue_ioctl`。
- 在 mutex 内取得一致 FIFO 状态快照，解锁后 `copy_to_user()`。
- 编写 `kqueue_ioctl_test.c`，验证空队列、写入后、重复查询、部分读取后状态。
- 验证未知 ioctl 返回 `ENOTTY`。
- 使用不可写地址验证查询返回 `EFAULT`，且内核不崩溃。
- 编写 `tests/run-kqueue-regression.sh`。
- 把 9 个普通功能测试和 1 个模块生命周期测试纳入统一回归。
- 在 ARM64 QEMU 中完成 `passed: 10`、`failed: 0`、退出码 0。
- 配置 QEMU `-s -S` 与 GDB 多架构调试。
- 命中 `start_kernel()`，加载模块符号，设置 `kqueue_write()` 和 `kqueue_read()` 条件断点。
- 验证 `abc` 的用户缓冲区、FIFO 数据量、`kfifo_from_user()`/`kfifo_to_user()`、调用栈和 ARM64 `x0` 返回值。
- 识别模块代码区符号与数据区符号地址的区别，并定位真实 FIFO 数据地址。

掌握要求：能解释 ioctl 参数、命令编号、用户指针、`copy_to_user()`、`ENOTTY`、`EFAULT`、Shell 汇总逻辑；GDB 数据符号重定位以理解为主。

### 第 7 天：功能基线与资源生命周期

用户当时要求回顾第 7 天并检查是否上传成功。检查发现：

- 之前的第 7 天对话已经实际完成 ARM64 QEMU 回归、模块生命周期、`release/fasync/ioctl` 和 SIGIO 教学。
- 原仓库没有独立的 `day07-summary.md`。
- `main` 分支当时比远程领先两笔提交。

随后读取上一任务的侧边终端记录，确认：

```text
passed: 10
failed: 0
regression_exit=0
aarch64
6.12.27
```

执行动作：

- 新增 `logs/day07-summary.md`。
- 更新 README 的每日记录范围和项目状态。
- 提交 `6c79fd5`。
- 首次 `git push` 因 GitHub HTTPS 未认证失败。
- 检查发现 GitHub CLI 已安装但未登录，也没有可用 SSH 密钥。
- 启动 `gh auth login --hostname github.com --web`。
- 用户完成一次性验证码授权。
- `gh auth setup-git` 配置 Git 使用 GitHub CLI 凭据。
- 推送成功，远程 `main` 与本地提交 `6c79fd5` 一致。

第 7 天记录明确写出：它是根据此前真实终端证据补记，不声称当天重新运行；长时间稳定性测试、干净环境复现仍未完成。

### 第 8 天：锁路径复查、部分拷贝故障和重复回归

用户先问“今天主要内容是什么”，得到的当天目标是：检查读写锁、等待和唤醒；进行有界重复回归；理解一次通过与重复稳定的区别。

用户随后要求自动完成当天能自动化的全部内容。执行过程：

1. 读取永久日志、README、回归脚本、启动脚本、驱动源码和用户测试。
2. 确认没有其他 QEMU 实例运行。
3. 发现本地主机模块与 rootfs 部署模块哈希不同，因此决定重新编译和部署。
4. 阅读 Linux 6.12.27 的 `lib/kfifo.c`，确认用户拷贝可能部分成功后返回 `EFAULT`，同时推进 FIFO 指针。
5. 新增 `kqueue_partial_fault_test.c`，使用两页映射和 `mprotect(PROT_NONE)` 让传输跨越保护页，验证部分写和部分读。
6. 将测试加入统一回归。
7. 新增 `repeat-qemu-regression.py`，实现独立快照 VM、镜像/部署文件 SHA-256 核验、每轮超时、逐轮日志、最终 dmesg 和正常关机检查。
8. 新增 `build-kqueue.sh` 统一构建模块、所有用户程序、部署 overlay 和 rootfs。
9. 重新编译模块与所有测试，重建镜像。
10. 首次自动回归遇到串口 `\r\r\n` 换行导致的标记匹配超时；保留日志并修正匹配逻辑，未误判为驱动失败。
11. 旧驱动真实复现：原有 10 项通过，新部分拷贝项失败，日志为：

```text
partial fault write: returned=-1 used=1 expected=1/1
passed: 10
failed: 1
```

12. 修复读写路径：只有 `ret != 0 && copied == 0` 才直接返回错误；部分成功继续唤醒并返回实际复制量。
13. 修复后连续 10 轮、每轮 11 项全部通过，共 110 项，完整会话约 80.275 秒。
14. 新增 `logs/day08-code-review.md`，记录每条路径的锁状态、等待/唤醒关系、问题和限制。
15. 新增 `logs/day08-summary.md`，更新 README 并提交 `bf03bc6`。
16. 推送到 GitHub 成功。

这次修复的教学结论：如果已经复制了部分字节，返回“完整失败”会让用户态不知道数据已经发生变化；应返回正的实际传输量并执行对应通知。完全没有传输时才返回 `EFAULT`。

### 第 9 天：专项并发、干净构建和交付收口

用户要求完成除 8 小时稳定性测试以外的所有剩余工作。执行动作：

#### 专项测试

- 新增 `kqueue_partial_wakeup_test.c`：部分写成功后唤醒阻塞读者；部分读成功后唤醒阻塞写者。
- 新增 `kqueue_multi_reader_test.c`：4 个读者消费 16,384 字节，校验字节内容；执行 1,000 次打开/关闭。
- 回归套件从 11 项扩展为 13 项。
- 当前镜像中 13 项专项回归通过。

#### 干净构建

- 生成可移植的 `configs/buildroot-aarch64-virt-defconfig`，以占位符代替旧绝对路径。
- 修改 `start-qemu.sh` 支持 `KQUEUE_IMAGES_DIR`。
- 修改回归器支持指定镜像目录、部署目录和预期测试数。
- 新增 `tests/clean-reproduce.sh`。
- 脚本校验固定 Buildroot 归档 SHA-256：`e5e58bff...866d69c`。
- 使用新工作区 `/home/zhaojinxin/driver-lab-clean-repro-174ffad`。
- 只复用 584 MiB、经过 Buildroot 哈希校验的下载缓存，不复用旧 `output/`、工具链、内核、模块、测试二进制和 rootfs。
- 从空 output 构建工具链、Linux 6.12.27、模块、用户程序和 rootfs。
- 构建耗时 1198 秒。
- 新镜像 `Image` SHA-256：`655bf730...d81dcdd2`。
- 新 rootfs SHA-256：`e683a991...19381334`。
- 新 ARM64 来宾显示 Linux 6.12.27。
- 新镜像 13 项回归：`passed: 13`、`failed: 0`、退出码 0，单轮回归约 9.733 秒。
- 保存 checkpoint、串口、逐轮日志、dmesg 和 summary JSON。

#### 最终交付材料

新增：

- `docs/interface.md`：设备、read/write、poll、SIGIO、ioctl、并发和生命周期接口约定。
- `docs/test-report.md`：环境、测试矩阵、历史回归、干净复现、结论和边界。
- `docs/known-limitations.md`：共享字节流、QEMU 边界、测试覆盖和未验证内容。
- `docs/demo.md`：3 分钟演示流程。
- `docs/interview-resume.md`：简历项目条目、面试问题、掌握程度。
- `docs/release-checklist.md`：v1.0 发布检查表。
- `logs/day09-summary.md`：当天实际学习和操作记录。

#### 8 小时工具准备

- 回归器增加 `--duration-hours`。
- 每轮结束原子更新 `checkpoint.json`。
- 最终写入 `summary.json`。
- 每轮有超时控制，失败时保留日志。
- 用 1 轮 13 项回归做了自检，结果 PASS。
- 用户明确“暂时做不了”8 小时测试，因此没有启动长测。

#### 版本提交

- `174ffad`：补充专项并发测试与干净复现入口。
- `c03484e`：完成干净复现与主项目交付收口。
- 创建并推送 `v1.0-rc1`，远程标签与提交 `c03484e` 一致。

## 四、主要 Git 提交和含义

| 提交 | 含义 |
|---|---|
| `05a2bd3` | 初始项目基础提交，包含命令行笔记 |
| `394ac3a` | 第 4 天阻塞/非阻塞 I/O、测试和日志 |
| `3c77718` | 第 5 天 poll、并发、SIGIO、环境和 README |
| `7b13a3c` | 补全前 3 天学习记录 |
| `6cd9cea` | 第 6 天 ioctl、统一回归和 GDB 调试 |
| `6c79fd5` | 补记第 7 天、更新 README并上传 |
| `bf03bc6` | 第 8 天部分拷贝缺陷修复、10 轮回归 |
| `174ffad` | 专项并发测试、可移植配置、干净复现入口 |
| `c03484e` | 干净复现证据、交付文档、检查点和候选版收口 |

当前远程 `main` 与 `v1.0-rc1` 指向 `c03484e599e90ee25505d0a853768ac5a099780f`。

## 五、对话中反复解释的技术要点

### 用户态到驱动

```text
用户 read/write/ioctl/poll
→ 系统调用
→ VFS 根据 fd 找到 struct file
→ file_operations 回调
→ kqueue_lab 驱动
→ mutex 保护 kfifo
→ 用户结果和 errno
```

### 阻塞读取

```text
加锁检查 FIFO
→ 有数据：持锁进入复制
→ 无数据且非阻塞：解锁并返回 EAGAIN
→ 无数据且阻塞：解锁并等待
→ 对端写入并唤醒
→ 重新加锁检查
→ 读取并唤醒写者
```

### 异步通知

```text
F_SETOWN
→ O_ASYNC
→ fasync_helper 登记订阅
→ 成功写入
→ kill_fasync 发送 SIGIO
→ 用户程序 read 取得真正数据
```

通知不携带 FIFO 数据；信号可能合并；收到通知后仍需处理 `read()` 和 `EAGAIN`。

### 模块生命周期

```text
insmod
→ module_init
→ misc_register
→ open
→ owner 持有模块引用
→ close/release
→ 撤销 fasync 订阅
→ rmmod
→ module_exit
→ misc_deregister
```

### 部分用户复制

`kfifo_from_user()`/`kfifo_to_user()` 可能先复制部分数据，再返回 `EFAULT` 并推进 FIFO。驱动必须根据 `copied` 判断：`copied > 0` 返回实际字节数并通知，`copied == 0` 才返回错误。

## 六、最终完成状态

已经完成：

- ARM64 Buildroot/QEMU 环境。
- miscdevice、kfifo、阻塞/非阻塞 read/write。
- mutex、等待队列、poll、SIGIO/fasync、ioctl。
- 零长度、部分传输、EAGAIN、EINTR、EFAULT、ENOTTY。
- 单/双生产者、4 读者、部分拷贝唤醒、1000 次打开关闭。
- 模块生命周期和 GDB 调试。
- 13 项回归、短时重复验证、干净环境构建复现。
- 接口、测试报告、限制、演示、简历和面试材料。
- GitHub `main` 与 `v1.0-rc1`。

尚未完成：

- 约 8 小时稳定性测试。
- 该测试完成后把 `v1.0-rc1` 升级为 `v1.0`。
- I²C/SMBus i2c-stub 扩展实验尚未开始；它不是当前字符设备主项目的阻塞项。

当前准确表述：主项目为候选交付版，功能、专项并发、干净构建和文档均已完成；长期稳定性测试工具已经准备但尚未执行。

## 六点五、后续对话节点逐条整理

在第 8 天及收口阶段，用户依次提出了以下要求，处理结果如下：

1. 用户询问“今天主要内容是什么”。已说明当天重点是并发读写、部分用户态拷贝故障、唤醒链路、专项回归和干净环境复现，并区分了可自动完成的工作与必须等待的长期稳定性测试。
2. 用户要求“先帮我执行今天内容里面你能帮我自动化完成的所有内容”。已自动完成代码审查、补充专项测试、构建测试程序、执行 10 轮回归、采集日志和生成汇总证据。
3. 用户询问“8 小时稳定性测试和干净环境复现怎么完成”。已解释启动前检查、后台运行、日志目录、checkpoint、summary、超时和结果判定；干净复现则使用空输出目录重新构建并在新 QEMU 环境执行回归。
4. 用户询问“整个项目还剩多少内容”。已按主项目与扩展实验拆分进度：主项目已接近交付，剩余主要是长期稳定性测试、最终版本升级和 I²C/SMBus 扩展。
5. 用户要求完成除 8 小时测试外的所有部分并详细讲解。已完成专项并发测试、部分拷贝修复、干净构建复现、测试报告、接口文档、限制说明、演示材料、简历/面试材料和发布检查表。
6. 用户要求阅读未完成部分并继续完成。已根据未完成清单继续执行干净环境复现和最终 runner 自检，并补齐交付证据。
7. 用户询问 8 小时稳定性测试是否必要。已说明它对长期资源泄漏、偶发竞态和持续运行信心有价值，但不是当前功能交付、简历材料或候选版本收口的硬阻塞；没有执行就不能宣称通过。
8. 用户表示“暂时做不了”。已保留 8 小时测试为明确待办，不伪造结果，并将项目状态固定为 `v1.0-rc1` 候选交付版。
9. 用户再次询问项目完成到哪里。已报告：功能、专项并发、干净构建、短时回归、文档和远端上传均已完成；剩余是 8 小时测试、测试后的 `v1.0` 升级，以及非阻塞的 I²C/SMBus 扩展。
10. 当前请求是将前置任务的所有对话和操作日志整理成 Markdown。本文件即为该请求的交付物，并保留原始日志、证据目录和 Git 历史作为逐项复核入口。

## 七、可复查文件索引

- 永久约定：`/home/zhaojinxin/linux_Ubuntu/PERMANENT_LOG.md`
- 每日记录：`driver-lab/logs/day01-summary.md` 至 `day09-summary.md`
- 最终测试报告：`driver-lab/docs/test-report.md`
- 接口约定：`driver-lab/docs/interface.md`
- 限制：`driver-lab/docs/known-limitations.md`
- 演示：`driver-lab/docs/demo.md`
- 简历/面试：`driver-lab/docs/interview-resume.md`
- 发布检查表：`driver-lab/docs/release-checklist.md`
- 干净复现脚本：`driver-lab/tests/clean-reproduce.sh`
- 构建脚本：`driver-lab/tests/build-kqueue.sh`
- 回归脚本：`driver-lab/tests/repeat-qemu-regression.py`、`run-kqueue-regression.sh`
- 原始 QEMU 证据：`driver-lab/logs/20261123T094116Z-clean-reproduction/`
- 原始专项证据：`driver-lab/logs/20261123T091845Z-final-specialized/`
- 历史 10 轮证据：`driver-lab/logs/20261123T090503Z-day08-after/`

## 八、创建本文档的操作记录

- 读取永久日志，确认用户长期教学、命令、记录和掌握程度要求。
- 读取每日摘要、README、测试报告、接口文档和限制文档。
- 读取 Git 历史，确认提交顺序、提交信息和最终候选版标签。
- 读取原始回归目录和 summary JSON，核对 13 项通过、干净构建耗时、镜像哈希、源码提交号和 QEMU 环境。
- 使用 `apply_patch` 创建本文件，没有修改测试代码或历史证据。
- 写入后应检查本文档内容；本文档本身尚未替代任何原始日志。
