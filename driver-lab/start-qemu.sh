#!/usr/bin/env bash
set -euo pipefail
lab_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
images="${KQUEUE_IMAGES_DIR:-$lab_dir/buildroot-2025.02.19/output/images}"
test -f "$images/Image"
test -f "$images/rootfs.ext4"
# Snapshot mode keeps the built rootfs unchanged between practice sessions.
# Files written inside this VM disappear on exit; keep source on the host.
exec qemu-system-aarch64 \
  -M virt -cpu cortex-a53 -m 512M -smp 2 -nographic \
  -snapshot -no-reboot \
  -kernel "$images/Image" \
  -append 'rootwait root=/dev/vda console=ttyAMA0' \
  -netdev user,id=eth0 \
  -device virtio-net-device,netdev=eth0 \
  -drive "file=$images/rootfs.ext4,if=none,format=raw,id=hd0" \
  -device virtio-blk-device,drive=hd0
