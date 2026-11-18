# kqueue_lab：内核 FIFO 读写实验

## 目标

实现一个 miscdevice：用户程序向 `/dev/kqueue_lab` 写入数据，驱动将数据保存到内核 FIFO；用户程序再从同一设备读取数据。FIFO 为空或已满时，驱动返回 `-EAGAIN`。

## 数据路径

```text
用户程序 write()
→ /dev/kqueue_lab
→ kqueue_write()
→ kfifo_from_user()
→ kqueue_fifo

用户程序 read()
→ /dev/kqueue_lab
→ kqueue_read()
→ kfifo_to_user()
→ 用户程序
```

`/dev/kqueue_lab` 是设备入口，不是普通文件。对它执行 `read()` 或 `write()` 时，内核会调用驱动中对应的回调；本实验的回调将数据读写到 `kqueue_fifo`。

## 实现要点

FIFO 容量为 4096 字节，并通过 mutex 保护：

```text
static DEFINE_KFIFO(kqueue_fifo, unsigned char, 4096);
static DEFINE_MUTEX(kqueue_fifo_lock);
```

- `kfifo_from_user()`：用户空间 → 内核 FIFO，用于 `write()`。
- `kfifo_to_user()`：内核 FIFO → 用户空间，用于 `read()`。
- `copied`：本次实际传输的字节数。
- `-EAGAIN`：当前无法读写，请稍后重试；本实验中对应 FIFO 为空，或 FIFO 没有可用写入空间。

## 验证结果

### 写入并读回 `abc`

写入：

```sh
printf 'abc' > /dev/kqueue_lab
```

内核日志：

```text
kqueue_lab: write requested=3, copied=3
```

读取：

```sh
dd if=/dev/kqueue_lab of=/tmp/kqueue-read.bin bs=1 count=3
```

驱动日志显示三次 `read requested=1, copied=1`。读取结果：

```sh
od -An -tx1 /tmp/kqueue-read.bin
cat /tmp/kqueue-read.bin
```

```text
61 62 63
abc
```

结果表明写入 FIFO 的 `abc` 被正确读回。

### FIFO 为空

读完 `abc` 后再次读取 1 字节：

```sh
dd if=/dev/kqueue_lab of=/tmp/kqueue-empty.bin bs=1 count=1
```

命令返回非零退出码（`1`），符合 `kqueue_read()` 在 `copied == 0` 时返回 `-EAGAIN` 的预期。

### FIFO 已满

向容量为 4096 字节的 FIFO 尝试写入 5120 字节：

```sh
dd if=/dev/zero of=/dev/kqueue_lab bs=1024 count=5
```

前 4 次写入共 4096 字节；第 5 次没有空间，驱动返回 `-EAGAIN`。随后读出已保存的数据：

```sh
dd if=/dev/kqueue_lab of=/tmp/kqueue-drain.bin bs=1024 count=4
wc -c < /tmp/kqueue-drain.bin
```

输出为 `4096`，说明 FIFO 中的 4096 字节均被完整读出。读完后再次读取 1 字节返回非零退出码，说明 FIFO 再次为空。

## 排错记录

- 【AI 错误】初版建议使用 `DECLARE_KFIFO`，但没有调用初始化操作，导致 `kfifo_from_user()` 失败，`write()` 返回错误。已改为 `DEFINE_KFIFO`，在定义时完成初始化。
- 【AI 判断失误】宿主机生成新模块后，仅在旧 QEMU 中重新 `insmod`，曾误判已经加载新版。实际上 QEMU 必须退出并重启，新的 rootfs 才会进入来宾系统。
- Buildroot 的 BusyBox `od` 不支持 `-t x1c`；改用 `od -An -tx1` 查看十六进制字节。

## 当前结论

`kqueue_lab` 已完成最小 FIFO 读写功能：

```text
写入成功
→ 可读取相同数据
→ FIFO 空返回 -EAGAIN
→ FIFO 满返回 -EAGAIN
→ 读出已保存的全部数据后 FIFO 恢复为空
```
