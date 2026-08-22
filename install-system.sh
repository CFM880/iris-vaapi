#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo ./install-system.sh" >&2
    exit 1
fi

src=/home/cfm880/sources/iris-vaapi/build/iris_drv_video.so
dst=/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so

[ -f "$src" ] || { echo "missing $src"; exit 1; }

install -m 0755 "$src" "$dst"
echo "installed $dst"
echo "test: LIBVA_DRIVER_NAME=iris vainfo"