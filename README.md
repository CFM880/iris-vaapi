# vpu-vaapi

**English** | [中文](README.zh.md)

An experimental, layered VA-API decode driver. It hands the parameters and slices submitted by
VA-API clients such as Chrome and FFmpeg to independent codec adapters, which reassemble them into
complete access units, then passes them to the hardware through a platform layer.
The current `qcom-iris` platform targets the Qualcomm SM8150 Iris1 stateful V4L2 decoder; the VA
frontend, codec adapters, scheduling/surface management, and platform device operations are
decoupled. Both new codecs and independent platform implementations can be added later.

Current version: `0.2.0`.

> The project is still experimental and is currently verified only against the Xiaomi Pad 5
> (`nabu`) and its companion kernel.

## Feature status

| Format | VA-API profile | Output | Status |
|---|---|---|---|
| H.264 | Constrained Baseline / Main / High | NV12 | Verified |
| HEVC | Main / Main10 | NV12 / P010 | Verified (8/10-bit) |
| VP9 | Profile 0 / Profile 2 | NV12 / P010 | Verified (8/10-bit) |

HEVC Main10 and VP9 Profile 2 have each passed three full 4K P010 bitstream test runs, with all
frames output successfully and no corrupt frames, so they count as supported VA-API decode
capabilities. H.264, HEVC, and VP9 support the stable DMA-BUF surfaces, asynchronous fences,
decode-order output, and end-of-stream frame release that Chrome needs. The experimental V4L2
CAPTURE direct path can be enabled with `VPU_DIRECT_CAPTURE=1`; the default remains the
well-verified stable surface-copy path.

## Dependencies

- Linux ARM64 with a matching `qcom-iris` driver;
- VA-API 1.23 ABI (verified with libva 2.22/2.23);
- GCC, pkg-config, make;
- Optional: `libvulkan-dev` (experimental Turnip DMA-BUF asynchronous copy);
- `/dev/video0`, a DRM render node, and `/dev/dma_heap/system`.

On Debian/Ubuntu systems:

```sh
sudo apt install build-essential pkg-config libva-dev libdrm-dev vainfo \
  libvulkan-dev
```

The kernel-side source and build instructions are in
[`nabu-iris`](https://github.com/CFM880/nabu-iris). For H.264/HEVC/VP9 4K playback, enable the
unified parameter:

```text
options qcom_iris cached_capture=1
```

## Build

```sh
git clone https://github.com/CFM880/iris-vaapi.git
cd iris-vaapi
make -j"$(nproc)"
LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH="$PWD/build" vainfo
```

By default the platform is auto-selected, and `/dev/video*` is scanned to identify decoders by the
Iris driver identifier, M2M capabilities, and compressed-input/raw-output formats, skipping cameras
and encoders and not relying on device numbering. Initialization fails when no decode device is
available. On multi-VPU systems or when debugging, you can select explicitly (`VPU_DEVICE` takes
precedence over auto-detection):

```sh
VPU_PLATFORM=qcom-iris VPU_DEVICE=/dev/video1 \
LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH="$PWD/build" vainfo
```

See [`docs/architecture.md`](docs/architecture.md) for the layering, the platform contract, and the
steps to add a platform.

You should see `VAEntrypointVLD` for H.264, HEVC Main/Main10, and VP9 Profile 0/Profile 2.

The driver advertises capabilities according to the CAPTURE formats the selected decode device
actually enumerates. If the kernel module has not been updated and does not provide P010, Main10 and
Profile 2 are hidden, avoiding per-frame client attempts that then fall back to software decode; in
that case, update and reload the companion `nabu-iris` module first.

To let FFmpeg use the current build without installing:

```sh
LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH="$PWD/build" \
ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 \
  -i /path/to/video.mp4 -an -f null -
```

H.264 uses asynchronous submission for VA surfaces that are not exported, in order to keep the
hardware decode pipeline running; surfaces already exported via DRM PRIME continue to do a
compatibility wait in `vaEndPicture`, preventing older Adreno from displaying the previous frame.
For diagnostics you can set `VPU_H264_SYNC_END=1` to restore per-frame synchronization for all
H.264 surfaces.

Setting `VPU_VULKAN_COPY=1` enables the experimental Turnip DMA-BUF copy engine: the V4L2 CAPTURE
buffer is exported via `VIDIOC_EXPBUF`, Vulkan asynchronously copies it to the stable surface
already imported by Chrome, and only after the GPU completes is the surface fence signaled and the
CAPTURE buffer reclaimed. If Vulkan is unavailable or submission fails, it automatically falls back
to a CPU `memcpy`. The CPU path is still the default; enabling it by default will be considered
after long Chrome playback and multi-tab verification.
Decode contexts on the same VA display share one Vulkan instance/device, so seeking, stream
switching, and multiple tabs do not reinitialize Turnip. The SM8150-specific startup environment can
also set `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.json` to avoid loading unrelated
ICDs.

## System install

```sh
make
sudo ./install-system.sh
```

The script installs the driver from the current repository's `build/` and installs the DMA-heap
udev rules and the `cached_capture` modprobe configuration. They take effect after reloading the
module or rebooting:

```sh
cat /sys/module/qcom_iris/parameters/cached_capture
LIBVA_DRIVER_NAME=vpu vainfo
```

## Chrome

Make sure the browser's GPU process inherits the driver selection:

```sh
export LIBVA_DRIVER_NAME=vpu
google-chrome --enable-features=VaapiVideoDecoder
```

Check `kVideoDecoderName` in `chrome://media-internals` and Video Acceleration in `chrome://gpu`.
Chrome versions and distribution launch arguments may differ; driver-side tracing is available with:

```sh
VPU_VAAPI_DEBUG=1 google-chrome --enable-logging=stderr
```

### Temporary workaround: Chrome 153 HEVC seek regression

Chrome **153** hardware HEVC decode has a Chromium regression
(`ExtendedVideoBitstreamValidation`; `H265Decoder::Reset()` clears `active_sps_`,
so every seek is misdetected as a configuration change). After seeking, the
compositor repeatedly shows stale pre-seek frames while the decoder keeps
running. It is HEVC-specific and independent of this driver (Chrome 152 and
H.264/VP9 are clean; 0% with the feature disabled). Until it is fixed upstream,
launch Chrome with:

```sh
google-chrome --disable-features=ExtendedVideoBitstreamValidation
```

or stay on Chrome 152. Details, reproduction and cross-platform results:
[`docs/chromium-bug-m153-hevc-seek.md`](docs/chromium-bug-m153-hevc-seek.md),
[`docs/hevc-seek-validation.md`](docs/hevc-seek-validation.md).

## Testing

[Fluster](https://github.com/fluendo/fluster) is used as the standard bitstream test benchmark,
uniformly comparing the output MD5 and end-to-end timing of the FFmpeg software, V4L2 M2M, and
VA-API paths.
The upstream version is pinned, test resources are downloaded automatically, and the logs and
environment metadata of each run are saved:

```sh
make check-fluster       # five vectors, three paths, serial smoke test
make check-fluster-full  # four complete upstream test suites
```

See the [Fluster testing guide](docs/fluster.md) for usage and the performance statistics scope, and
the [Fluster test report](benchmark-results/fluster-baseline.md) for measured results on the three
paths. Timeouts and decode errors still occur; earlier full-bitstream/throughput verification does
not mean the conformance suite passes.

The pure parameter rebuild test needs no hardware:

```sh
make check
```

Hardware tests require user-provided bitstreams and contain no local absolute paths:

```sh
./build/test_v4l2_dec stream.h264 3840 2160
./build/test_va_decode stream.h264
./build/test_va_stress stream.h264 700
./build/test_hevc_au stream.h265
./build/test_va_vp9 stream.ivf
./build/test_hevc_au main10.h265 3840 2160 p010
./build/test_v4l2_vp9 profile2.ivf 3840 2160 p010
./build/test_surface_fence /dev/video0
```

## Performance comparison

The following is a maximum-throughput test on Xiaomi Pad 5 (SM8150) with 4K60 full bitstreams. CPU
is expressed as 100% = a single core fully loaded, and RSS is the process peak; V4L2 and VA-API are
run alternately within the same spec. H.264 uses the current asynchronous submission
implementation, and the other results come from the initial full benchmark.

| Spec | Bit depth | Path | Decode speed | CPU | Peak RSS |
|---|---:|---|---:|---:|---:|
| H.264 High | 8-bit | V4L2 | 167.55 fps | 30.24% | 415.84 MiB |
| H.264 High | 8-bit | VA-API | 166.95 fps | 59.28% | 544.30 MiB |
| HEVC Main | 8-bit | V4L2 | 176.30 fps | 31.71% | 411.63 MiB |
| HEVC Main | 8-bit | VA-API | 173.83 fps | 52.29% | 675.49 MiB |
| VP9 Profile 0 | 8-bit | V4L2 | 146.45 fps | 35.81% | 516.83 MiB |
| VP9 Profile 0 | 8-bit | VA-API | 144.49 fps | 44.91% | 556.79 MiB |
| HEVC Main10 | 10-bit | V4L2 P010 | 148.29 fps | 26.54% | 798.35 MiB |
| HEVC Main10 | 10-bit | VA-API | 148.55 fps | 85.70% | 937.84 MiB |
| VP9 Profile 2 | 10-bit | V4L2 P010 | 116.33 fps | 20.51% | 739.11 MiB |
| VP9 Profile 2 | 10-bit | VA-API | 113.82 fps | 42.09% | 798.59 MiB |

The VA-API vs V4L2 throughput difference for the current five specs is no more than 2.2%. The H.264
asynchronous optimization improved VA-API from 65.55 fps to 166.95 fps; although CPU utilization is
higher at maximum throughput, total CPU time for the full bitstream dropped from 8.66 seconds to
6.38 seconds. The experimental Vulkan copy path has reduced the stable-surface full-frame CPU copy
to zero; the forced readback verification of 120 frames of H.264 4K matches the CPU path per frame,
with total CPU time reduced by about 7% and wall-clock time increased by about 2%. The 600-frame warm
null sink test reduced total CPU time from about 2.69 seconds to 2.47 seconds. When the default
Vulkan loader scans all ICDs, the Turnip path increases RSS by about 64 MiB; after limiting to the
Freedreno ICD, the local increment is about 15 MiB. Turnip's first initialization still has a
noticeable CPU cost, so it is currently mainly used to validate the "trade GPU for CPU" direction
and is not yet the default configuration.

The raw data and reproducible scripts are in [`benchmark-results/`](benchmark-results/) and
[`benchmarks/`](benchmarks/); the 10-bit V4L2 test program maps the entire input file, so the raw
RSS in the table includes the input file size, and the CSV also has a corrected value after
subtracting the input mapping.

The detailed implementation process, benchmarks, and historical troubleshooting records are kept in
[`docs/development-notes.md`](docs/development-notes.md). For Chromium integration points, see
[`docs/chromium-integration.md`](docs/chromium-integration.md).

For the complete steps to bring up the kernel, firmware, VA-API, FFmpeg, and Chrome from a fresh
system, see [`中文`](docs/video-decode-setup.md) / [`English`](docs/video-decode-setup.en.md).

## Known limitations

- 10-bit P010 decode requires the companion `nabu-iris` kernel module; HDR metadata, tone mapping,
  and the final display result still depend on Chrome, the compositor, and the display chain;
- Chrome 153 hardware HEVC seek shows stale pre-seek frames due to a Chromium
  `ExtendedVideoBitstreamValidation` regression; run Chrome with
  `--disable-features=ExtendedVideoBitstreamValidation` or use Chrome 152 (see the Chrome section);
- FFmpeg's `hevc_v4l2m2m` and `vp9_v4l2m2m` frontends currently do not select a P010 CAPTURE for
  10-bit input, and may exit successfully while outputting 0 frames; the VA-API path and the
  repository's explicit-P010 V4L2 test programs are not affected by this limitation;
- the default stable-surface path does a single CPU copy from CAPTURE to DMA-BUF; the companion
  kernel's `cached_capture=1` speeds up MMAP CAPTURE reads for H.264/HEVC/VP9; `VPU_VULKAN_COPY=1`
  can switch to the experimental asynchronous GPU copy, but increases Turnip memory overhead;
- `VPU_DIRECT_CAPTURE` is still experimental and is not recommended as the default release
  configuration;
- abnormally terminating an old kernel session may cause a firmware timeout, requiring a reload of
  `qcom_iris`;
- a `nabu-iris` kernel module matching this repository's features is required; replacing only the
  userspace driver is not enough.

## License

This project is released under GPL-2.0-or-later; see [COPYING](COPYING) for details.
