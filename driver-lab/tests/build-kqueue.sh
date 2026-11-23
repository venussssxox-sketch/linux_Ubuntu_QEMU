#!/usr/bin/env bash
# Rebuild the module, all C tests, deployment overlay, and guest rootfs.
set -euo pipefail
lab_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$lab_dir"
buildroot="$lab_dir/buildroot-2025.02.19"
cross="$buildroot/output/host/bin/aarch64-buildroot-linux-gnu-"
make -C "$buildroot/output/build/linux-6.12.27" ARCH=arm64 \
    CROSS_COMPILE="$cross" M="$lab_dir/drivers/kqueue_lab" modules
mkdir -p userspace/build rootfs-overlay/root
for source_file in userspace/*.c; do
    test_name=$(basename "$source_file" .c)
    "${cross}gcc" -Wall -Wextra -Wpedantic -O2 "$source_file" \
        -o "userspace/build/$test_name"
    cp "userspace/build/$test_name" "rootfs-overlay/root/$test_name"
done
cp drivers/kqueue_lab/kqueue_lab.ko rootfs-overlay/root/kqueue_lab.ko
cp tests/run-kqueue-regression.sh rootfs-overlay/root/run-kqueue-regression.sh
make -C "$buildroot"
