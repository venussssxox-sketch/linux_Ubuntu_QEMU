# kqueue_lab 用户态 C 测试

## 目的

使用 ARM64 用户态 C 程序验证 `/dev/kqueue_lab` 的基本数据往返，以及 FIFO 为空时的非阻塞错误语义。

## 测试程序

- 源码：`userspace/kqueue_test.c`
- 宿主机编译产物：`userspace/build/kqueue_test`
- 来宾程序：`/root/kqueue_test`

程序先向设备写入 3 字节 `abc`，读取并按字节比较读回的数据；随后再次读取已空的 FIFO，要求用户态 `read()` 返回 `-1` 且 `errno` 为 `EAGAIN`。

## 构建与部署

在宿主机使用 Buildroot 的 ARM64 交叉编译器：

```sh
buildroot-2025.02.19/output/host/bin/aarch64-buildroot-linux-gnu-gcc \
    -Wall -Wextra -Wpedantic -O2 \
    userspace/kqueue_test.c \
    -o userspace/build/kqueue_test
```

`file userspace/build/kqueue_test` 确认为 `ELF 64-bit`、`ARM aarch64`。将程序复制到 `rootfs-overlay/root/kqueue_test` 后运行 Buildroot `make` 重建根文件系统。

## 来宾验证

```sh
insmod /root/kqueue_lab.ko
/root/kqueue_test
echo $?
```

实际输出：

```text
kqueue_lab: device opened
kqueue_lab: write requested=3, copied=3
kqueue_lab: read requested=3, copied=3
PASS: empty queue returned EAGAIN
kqueue_lab: device released
PASS: wrote and read back 3 bytes: abc
0
```

## 结论

- FIFO 有数据时，用户态 `read()` 成功返回 3 字节；读回的数据与写入的 `abc` 完全一致。
- 第一次读取消费全部数据后，第二次用户态 `read()` 返回 `-1`，并验证 `errno == EAGAIN`。
- 测试程序退出码为 0，表示两项断言均通过。

驱动只记录成功读写，因此空 FIFO 的 `EAGAIN` 不会额外生成 `read requested` 内核日志；该行为由用户态测试程序直接检查返回值和 `errno`。

## 零长度读取行为（待修复）

在测试程序中加入 `read(fd, buffer, 0)` 后，来宾实际输出：

```text
zero-length read: expected=0 actual=-1 errno=Resource temporarily unavailable
```

测试程序退出码为 `1`。这表明当前 `kqueue_read()` 没有区分“请求读取 0 字节”和“FIFO 为空”：两种情况都会落入 `copied == 0` 分支并返回 `-EAGAIN`。后续需要在读、写回调中对 `count == 0` 直接返回 0，再复测零长度读写不改变 FIFO 状态。

## 零长度读取修复后的复测

在 `kqueue_read()` 和 `kqueue_write()` 开头加入 `count == 0` 时直接返回 0 的分支后，重新编译模块和 ARM64 测试程序、更新 overlay 并重建 rootfs。来宾实际输出：

```text
kqueue_lab: device opened
kqueue_lab: write requested=3, copied=3
kqueue_lab: read requested=3, copied=3
kqueue_lab: device released
PASS: zero-length read returned 0
PASS: wrote and read back 3 bytes: abc
PASS: empty queue returned EAGAIN
```

随后 `echo $?` 输出 `0`。这说明零长度读取返回 0，后续普通读取仍成功读回 `abc`，证明零长度读取没有消费 FIFO 数据；数据消费完毕后，非零长度读取仍以用户态 `-1` 和 `errno == EAGAIN` 报告空 FIFO。

## 零长度写入复测

用户态测试程序增加 `write(fd, message, 0)` 检查后，来宾实际输出：

```text
kqueue_lab: device opened
kqueue_lab: write requested=3, copied=3
kqueue_lab: read requested=3, copied=3
kqueue_lab: device released
PASS: zero-length write returned 0
PASS: zero-length read returned 0
PASS: wrote and read back 3 bytes: abc
PASS: empty queue returned EAGAIN
```

随后 `echo $?` 输出 `0`。零长度写入、零长度读取均返回 0；后续普通写入和读取仍可完整往返 `abc`，且 FIFO 被消费完后仍正确报告 `EAGAIN`。
