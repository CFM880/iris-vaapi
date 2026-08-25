# iris-vaapi

面向 Qualcomm SM8150 Iris1 的实验性 VA-API 驱动。它把 Chrome、FFmpeg 等
VA-API 客户端提交的参数和 slice 重组成完整访问单元，再交给 `qcom-iris`
stateful V4L2 解码器。

当前版本：`0.1.0`。

> 项目仍处于实验阶段，目前只针对 Xiaomi Pad 5（`nabu`）及配套内核验证。

## 功能状态

| 格式 | VA-API profile | 输出 | 状态 |
|---|---|---|---|
| H.264 | Constrained Baseline / Main / High | NV12 | 已验证 |
| HEVC | Main 8-bit | NV12 | 已验证 |
| VP9 | Profile 0 | NV12 | 已验证 |

H.264 与 HEVC 支持 Chrome 所需的稳定 DMA-BUF surface、异步 fence、解码顺序
输出和片尾帧释放。实验性的 V4L2 CAPTURE 直连可通过
`IRIS_DIRECT_CAPTURE=1` 启用，默认仍使用已充分验证的稳定 surface 拷贝路径。

## 依赖

- Linux ARM64，带匹配的 `qcom-iris` 驱动；
- VA-API 1.23 ABI（已使用 libva 2.22/2.23 验证）；
- GCC、pkg-config、make；
- `/dev/video0`、DRM render node 和 `/dev/dma_heap/system`。

在 Debian/Ubuntu 系统上：

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev vainfo
```

内核侧源码和构建方法位于
[`nabu-iris`](https://github.com/CFM880/nabu-iris)。H.264/HEVC 4K 播放应启用
统一参数：

```text
options qcom_iris cached_capture=1
```

## 构建

```sh
git clone https://github.com/CFM880/iris-vaapi.git
cd iris-vaapi
make -j"$(nproc)"
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH="$PWD/build" vainfo
```

预期能看到 H.264、HEVC Main 和 VP9 Profile 0 的 `VAEntrypointVLD`。

无需安装即可让 FFmpeg 使用当前构建：

```sh
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH="$PWD/build" \
ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 \
  -i /path/to/video.mp4 -an -f null -
```

## 系统安装

```sh
make
sudo ./install-system.sh
```

脚本从当前仓库的 `build/` 安装驱动，并安装 DMA-heap udev 规则及
`cached_capture` modprobe 配置。模块重新加载或重启后生效：

```sh
cat /sys/module/qcom_iris/parameters/cached_capture
LIBVA_DRIVER_NAME=iris vainfo
```

## Chrome

确保浏览器 GPU 进程继承驱动选择：

```sh
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-features=VaapiVideoDecoder
```

在 `chrome://media-internals` 中检查 `kVideoDecoderName`，在 `chrome://gpu` 中
检查 Video Acceleration。Chrome 版本和发行版启动参数可能不同；驱动侧跟踪可用：

```sh
IRIS_VAAPI_DEBUG=1 google-chrome --enable-logging=stderr
```

## 测试

纯参数重建测试不需要硬件：

```sh
make check
```

硬件测试需要用户提供码流，不包含任何本机绝对路径：

```sh
./build/test_v4l2_dec stream.h264 3840 2160
./build/test_va_decode stream.h264
./build/test_va_stress stream.h264 700
./build/test_hevc_au stream.h265
./build/test_va_vp9 stream.ivf
./build/test_surface_fence /dev/video0
```

详细的实现过程、基准和历史排障记录保留在
[`docs/development-notes.md`](docs/development-notes.md)。Chromium 交互点见
[`docs/chromium-integration.md`](docs/chromium-integration.md)。

从全新系统打通内核、固件、VA-API、FFmpeg 和 Chrome 的完整步骤见
[`中文`](docs/video-decode-setup.md) / [`English`](docs/video-decode-setup.en.md)。

## 已知限制

- 仅验证 SM8150 Iris1 和 NV12 8-bit；HEVC Main10/P010 尚未完成；
- stable-surface 路径会做一次 CAPTURE 到 DMA-BUF 的 CPU 拷贝；
- `IRIS_DIRECT_CAPTURE` 仍是实验功能，不建议作为默认发布配置；
- 非正常终止旧内核会话可能使固件超时，需要重载 `qcom_iris`；
- 需要与本仓库功能匹配的 `nabu-iris` 内核模块，单独替换用户态驱动不够。

## 许可证

本项目以 GPL-2.0-or-later 发布，详见 [COPYING](COPYING)。
