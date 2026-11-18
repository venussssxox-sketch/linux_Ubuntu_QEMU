#!/usr/bin/env bash
set -euo pipefail
lab_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
br="$lab_dir/buildroot-2025.02.19"
kernel_version="$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="\(.*\)"$/\1/p' "$br/.config")"
kernel_dir="$br/output/build/linux-$kernel_version"
cross="$br/output/host/bin/aarch64-buildroot-linux-gnu-"
test -x "${cross}gcc"
test -f "$kernel_dir/Module.symvers"
make -C "$kernel_dir" ARCH=arm64 CROSS_COMPILE="$cross" \
  M="$lab_dir/env-check" modules
mkdir -p "$lab_dir/rootfs-overlay/root"
cp "$lab_dir/env-check/envcheck.ko" "$lab_dir/rootfs-overlay/root/envcheck.ko"
echo 'Module built and copied to the configured rootfs overlay. Run make in Buildroot to update the image.'
