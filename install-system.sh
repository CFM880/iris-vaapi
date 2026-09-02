#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo ./install-system.sh" >&2
    exit 1
fi

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
driver_dir=$(pkg-config --variable=driverdir libva 2>/dev/null || true)
[ -n "$driver_dir" ] || driver_dir=/usr/lib/aarch64-linux-gnu/dri

src=$repo_dir/build/vpu_drv_video.so
dst=$driver_dir/vpu_drv_video.so
qcom_config_dir=$repo_dir/src/platform/qcom/config
rule=/etc/udev/rules.d/99-qcom-iris-dmaheap.rules
modprobe_conf=/etc/modprobe.d/99-qcom-iris.conf

[ -f "$src" ] || { echo "missing $src"; exit 1; }

install -m 0755 "$src" "$dst"
echo "installed $dst"

install -m 0644 "$qcom_config_dir/99-qcom-iris-dmaheap.rules" "$rule"
chmod 0666 /dev/dma_heap/system 2>/dev/null || true
udevadm control --reload-rules 2>/dev/null || true
echo "installed $rule (dma_heap/system now writable)"

install -m 0644 "$qcom_config_dir/99-qcom-iris.conf" "$modprobe_conf"
echo "installed $modprobe_conf (cacheable H.264/HEVC/VP9 CAPTURE enabled on next module load)"
echo "test: LIBVA_DRIVER_NAME=vpu VPU_PLATFORM=qcom-iris vainfo"
