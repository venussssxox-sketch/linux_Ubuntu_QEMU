# Day 8 总结：并发路径复查、部分拷贝故障修复与重复回归

日期：2026-11-23。

## 完成内容

- 复查 read/write 的加锁、空满判断、解锁等待、信号返回、唤醒与重新判断路径。
- 复查 poll 先登记后读取状态，以及 ioctl 持锁快照、解锁复制的顺序。
- 根据 Linux 6.12.27 的 kfifo 实现发现部分用户拷贝故障处理缺陷，新增测试并先复现再修复。
- 重新编译驱动与所有 C 测试，更新根文件系统；新增统一构建入口。
- 新增有超时、有文件哈希核验、有每轮证据与退出码的主机端重复回归入口。
- 在独立 ARM64 QEMU 快照来宾完成 10 轮完整回归。

## 实际问题与修复

旧驱动在 kfifo 返回 EFAULT 时直接退出，但部分复制可能已经推进 FIFO 指针。新增测试在 FIFO 环形边界及用户内存保护页边界发起两字节传输，强制第一字节成功、第二字节失败。

修复前实际输出：

```text
partial fault write: returned=-1 used=1 expected=1/1
passed: 10
failed: 1
```

修复后的读写路径只在 `ret != 0 && copied == 0` 时直接返回错误，部分成功继续唤醒并返回实际复制量。新增测试验证部分写入和部分读取均返回 1，FIFO 状态与剩余字节内容正确。

自动化工具首轮遇到串口重复回车导致标记匹配超时，修正换行兼容后重新运行。工具超时与驱动回归失败的证据分别保留。

## 验证结果

- 环境：QEMU ARM64 virt，2 vCPU、512 MiB，Linux 6.12.27。
- 每轮 10 个普通测试与 1 个生命周期测试，共 11 项。
- 10 轮均为 passed=11、failed=0、exit_code=0，共 110 项测试执行通过。
- 完整会话耗时 80.275 秒，包括启动、文件核验、测试、日志采集和关机；不是纯测试耗时。
- 每轮超时上限 120 秒；没有触发超时。
- 每轮生产者/消费者用例检查 1024 字节顺序与 6000 A + 6000 B 字节分布，10 轮共检查该用例的数据 130240 字节；不是全部测试的数据总量。
- 镜像内模块、所有 kqueue 测试及回归脚本与主机 overlay 的 SHA-256 一致。
- 最终模块不存在于 /proc/modules，QEMU 正常关机、退出码 0。
- 最终 dmesg 未匹配 BUG、WARNING、Oops、Kernel panic 或 general protection fault 标记。日志扫描不等同于证明不存在任何并发缺陷。

## 原始证据

- `20261123T090326Z-day08-before/`：首次编排工具串口匹配超时，尚未运行回归。
- `20261123T090414Z-day08-before/`：旧驱动实际失败，原有 10 项通过、新项失败。
- `20261123T090503Z-day08-after/`：修复后串口、10 轮日志、最终 dmesg、summary.json。
- `day08-build-before.log`、`day08-build-after.log`：镜像构建日志。
- `day08-build-entrypoint.log`：统一构建入口实际执行成功；重建后全部部署文件哈希与通过测试的版本一致。
- JSON 中 git_head 是测试启动前的提交；当时修复尚在工作区，具体测试源码和部署二进制由 source_sha256 与 deployed_sha256 标识。

## 文件与复现

- `drivers/kqueue_lab/kqueue_lab.c`：两处部分拷贝错误处理修复。
- `userspace/kqueue_partial_fault_test.c`：新回归用例。
- `tests/run-kqueue-regression.sh`：纳入新增测试。
- `tests/build-kqueue.sh`：统一模块、用户态及镜像构建。
- `tests/repeat-qemu-regression.py`：独立虚拟机重复回归、超时和证据采集。
- `logs/day08-code-review.md`：锁状态检查、生命周期与测试边界。
- README：更新当前进度与复现命令。

从已有 Buildroot 构建环境重跑：

```bash
bash /home/zhaojinxin/linux_Ubuntu/driver-lab/tests/build-kqueue.sh
python3 /home/zhaojinxin/linux_Ubuntu/driver-lab/tests/repeat-qemu-regression.py --rounds 10 --timeout 120 --label regression
```

## 下一步与掌握程度

自动化已经完成源码检查与本次重复验证；学习者仍需亲自解释锁状态、唤醒后的竞争，以及为什么部分传输优先返回字节数。驱动修改要求达到能解释和修改；内存保护页测试以理解为主；编排脚本要求能调整轮数与超时、查看失败日志。

仍待完成：部分故障场景下等待者唤醒的专门并发用例、多读者专项、约 8 小时稳定性测试、干净环境复现与最终交付。当前结果不用于宣称真实 ARM 硬件性能或所有并发时序均已覆盖。
