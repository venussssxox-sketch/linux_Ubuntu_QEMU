#!/usr/bin/env bash
# Build and test the project without reusing any compiled output.
set -euo pipefail

lab_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace=""
archive="$lab_dir/buildroot-2025.02.19.tar.xz"
download_cache=""
expected_archive_sha="e5e58bff1da91f022f60cefdfe280140c08023cd0aeec1e5f6f9da0ea866d69c"

usage()
{
    printf 'usage: %s --workspace DIR [--archive FILE] [--download-cache DIR]\n' "$0"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --workspace)
            workspace="$2"
            shift 2
            ;;
        --archive)
            archive="$2"
            shift 2
            ;;
        --download-cache)
            download_cache="$2"
            shift 2
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$workspace" ]; then
    usage >&2
    exit 2
fi
if [ -e "$workspace" ]; then
    printf 'workspace already exists: %s\n' "$workspace" >&2
    exit 2
fi
if [ ! -f "$archive" ]; then
    printf 'Buildroot archive not found: %s\n' "$archive" >&2
    exit 2
fi

actual_sha=$(sha256sum "$archive" | awk '{print $1}')
if [ "$actual_sha" != "$expected_archive_sha" ]; then
    printf 'Buildroot archive SHA-256 mismatch\nexpected: %s\nactual:   %s\n' \
        "$expected_archive_sha" "$actual_sha" >&2
    exit 1
fi

mkdir -p "$workspace"
workspace="$(cd -- "$workspace" && pwd)"
source_dir="$workspace/buildroot-2025.02.19"
output_dir="$workspace/output"
overlay_dir="$workspace/rootfs-overlay"
project_copy="$workspace/project-source"
generated_defconfig="$workspace/generated-buildroot-defconfig"
build_log="$workspace/clean-build.log"
summary="$workspace/clean-reproduction-summary.txt"
start_epoch=$(date +%s)

tar -xf "$archive" -C "$workspace"
mkdir -p "$overlay_dir/root" "$output_dir" \
    "$project_copy/drivers/kqueue_lab" "$project_copy/include" \
    "$project_copy/userspace" "$project_copy/tests"
cp "$lab_dir/drivers/kqueue_lab/Makefile" \
    "$lab_dir/drivers/kqueue_lab/kqueue_lab.c" \
    "$project_copy/drivers/kqueue_lab/"
cp "$lab_dir/include/kqueue_lab_ioctl.h" "$project_copy/include/"
cp "$lab_dir"/userspace/*.c "$project_copy/userspace/"
cp "$lab_dir/tests/run-kqueue-regression.sh" "$project_copy/tests/"
sed -e "s|@ROOTFS_OVERLAY@|$overlay_dir|" \
    -e "s|@KERNEL_CONFIG@|$lab_dir/configs/linux-aarch64-virt-debug.config|" \
    "$lab_dir/configs/buildroot-aarch64-virt-defconfig" > "$generated_defconfig"

make_args=(-C "$source_dir" "O=$output_dir")
if [ -n "$download_cache" ]; then
    if [ ! -d "$download_cache" ]; then
        printf 'download cache is not a directory: %s\n' "$download_cache" >&2
        exit 2
    fi
    make_args+=("BR2_DL_DIR=$(cd -- "$download_cache" && pwd)")
fi

{
    printf 'git_head=%s\n' "$(git -C "$lab_dir" rev-parse HEAD)"
    printf 'archive_sha256=%s\n' "$actual_sha"
    printf 'workspace=%s\n' "$workspace"
    printf 'build_started=%s\n' "$(date --iso-8601=seconds)"
} > "$summary"

make "${make_args[@]}" BR2_DEFCONFIG="$generated_defconfig" defconfig \
    > "$build_log" 2>&1
make "${make_args[@]}" >> "$build_log" 2>&1

cross="$output_dir/host/bin/aarch64-buildroot-linux-gnu-"
kernel_build="$output_dir/build/linux-6.12.27"
make -C "$kernel_build" ARCH=arm64 CROSS_COMPILE="$cross" \
    M="$project_copy/drivers/kqueue_lab" modules >> "$build_log" 2>&1

mkdir -p "$workspace/userspace-build"
for source_file in "$project_copy"/userspace/*.c; do
    test_name=$(basename "$source_file" .c)
    "${cross}gcc" -Wall -Wextra -Wpedantic -O2 "$source_file" \
        -o "$workspace/userspace-build/$test_name" >> "$build_log" 2>&1
    cp "$workspace/userspace-build/$test_name" "$overlay_dir/root/$test_name"
done
cp "$project_copy/drivers/kqueue_lab/kqueue_lab.ko" \
    "$overlay_dir/root/kqueue_lab.ko"
cp "$project_copy/tests/run-kqueue-regression.sh" \
    "$overlay_dir/root/run-kqueue-regression.sh"

make "${make_args[@]}" >> "$build_log" 2>&1

python3 "$lab_dir/tests/repeat-qemu-regression.py" \
    --rounds 1 --expected-tests 13 --timeout 120 \
    --images-dir "$output_dir/images" \
    --deployment-dir "$overlay_dir/root" \
    --label clean-reproduction

end_epoch=$(date +%s)
{
    printf 'build_finished=%s\n' "$(date --iso-8601=seconds)"
    printf 'elapsed_seconds=%s\n' "$((end_epoch - start_epoch))"
    printf 'kernel_sha256=%s\n' \
        "$(sha256sum "$output_dir/images/Image" | awk '{print $1}')"
    printf 'rootfs_sha256=%s\n' \
        "$(sha256sum "$output_dir/images/rootfs.ext4" | awk '{print $1}')"
    printf 'result=PASS\n'
} >> "$summary"

printf 'PASS: clean build and 13-test ARM64 regression\n%s\n' "$summary"
