#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo ./install-system.sh" >&2
    exit 1
fi

src=/home/cfm880/sources/iris-vaapi/build/iris_drv_video.so
dst=/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so
rule=/etc/udev/rules.d/99-iris-dmaheap.rules
modprobe_conf=/etc/modprobe.d/99-iris-vaapi.conf

[ -f "$src" ] || { echo "missing $src"; exit 1; }

install -m 0755 "$src" "$dst"
echo "installed $dst"

install -m 0644 "$(dirname "$0")/tools/99-iris-dmaheap.rules" "$rule"
chmod 0666 /dev/dma_heap/system 2>/dev/null || true
udevadm control --reload-rules 2>/dev/null || true
echo "installed $rule (dma_heap/system now writable)"

install -m 0644 "$(dirname "$0")/tools/99-iris-vaapi.conf" "$modprobe_conf"
echo "installed $modprobe_conf (cacheable H.264 CAPTURE enabled on next module load)"
echo "test: LIBVA_DRIVER_NAME=iris vainfo"
