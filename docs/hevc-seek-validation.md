# HEVC seek 旧帧问题：验证指南（Intel / 跨平台）

配套的 bug 报告见 [`chromium-bug-m153-hevc-seek.md`](chromium-bug-m153-hevc-seek.md)。
本文用于在 **Intel 等其它 VA-API 平台**复现，以及 Chromium 修复后回测。

---

## 1. 结论回顾

- 症状：Chrome **153** 上硬解 HEVC，播放中拖动进度条后，画面周期性跳回 seek
  之前的旧帧；解码器仍在正常出帧（`totalVideoFrames` 增长）。
- 根因：M153 的 feature **`ExtendedVideoBitstreamValidation`**（默认开）+
  `H265Decoder::Reset()` 误清 `active_sps_`，导致每次 seek 误判配置变更。
- 与驱动无关；**仅 HEVC**，H.264/VP9/AV1 不受影响。

预期验证结果：

| 配置 | stale repeats |
|---|---|
| Chrome 153，默认（feature 开） | 约 70–90% |
| Chrome 153，`--disable-features=ExtendedVideoBitstreamValidation` | 0–5% |
| Chrome 152 | 0–5% |

---

## 2. 平台前置检查（Intel 10 代 i9）

10 代 i9 核显：
- Comet Lake：UHD 630（Gen9.5）
- Ice Lake：Iris Plus（Gen11）

两者都支持 HEVC 8/10bit 硬解。

```sh
# 1) 核显在不在
lspci -nnk | grep -iA3 vga

# 2) VA-API 驱动（二选一；10 代/iHD 推荐）
sudo apt install intel-media-va-driver      # iHD，Comet Lake/Ice Lake 都行
# 或 sudo apt install i965-va-driver        # 旧驱动，仅到 Gen9.5

# 3) 验证 HEVC 支持
vainfo --display drm --device /dev/dri/renderD128
#   需要看到：
#     VAProfileHEVCMain      : VAEntrypointVLD
#     VAProfileHEVCMain10    : VAEntrypointVLD
```

如果没有 `VAProfileHEVC*`，说明该核显/驱动不支持 HEVC 硬解，换机器或走 Windows D3D11 路径验证。

---

## 3. 素材与 harness

- 素材：4K HEVC 8bit MP4，建议 ≥30s（harness 每 4s seek 一次，共 7 次）。
  仓库外的素材路径按需替换。
- harness：`iris-vaapi/benchmarks/seek_harness/`
  - `seq_server.py`：本地 HTTP（支持 Range）+ 收集页面上报
  - `seek_test.html`：播放、定时 seek、上报 48x27 灰度哈希
  - `analyze.py`：统计 seek 后「与 seek 前重复」的比例（`stale_repeats`）

> Intel 上没有 iris-vaapi 的帧号条码（`VPU_FRAME_STAMP`），所以 `sn`/`stale_serial`
> 字段无意义，**只看 `stale_repeats`**。

---

## 4. 复现 / 度量

```sh
cd iris-vaapi
VID=/path/to/4k-hevc-8bit.mp4

# 起服务
SEEK_VIDEO="$VID" SEEK_LOG=/tmp/seq.log SEEK_PORT=8756 \
  python3 benchmarks/seek_harness/seq_server.py &

# Chrome 153，硬解（Intel 一般默认已开，必要时加 flag）
LIBVA_DRIVER_NAME=iHD \
/opt/google/chrome/google-chrome \
  --user-data-dir=/tmp/chrome-seek --ozone-platform=wayland --start-fullscreen \
  --enable-features=VaapiVideoDecoder,VaapiVideoEncoder,PlatformHEVCDecoderSupport \
  --ignore-gpu-blocklist \
  http://127.0.0.1:8756/

# 播 ~36s 后关掉 Chrome，分析
python3 benchmarks/seek_harness/analyze.py /tmp/seq.log
```

> **务必用真实二进制。** 若本机存在会注入
> `--disable-features=ExtendedVideoBitstreamValidation` 的 wrapper（典型是
> `~/.local/bin/google-chrome-stable`，因 `~/.local/bin` 在 `PATH` 中先于
> `/usr/bin` 而覆盖真实二进制），那么所有“默认”运行其实都被强制关掉了 feature，
> 回归会看起来像修好了。复现与 A/B 请直接调用
> `/opt/google/chrome/google-chrome`。实测 `153.0.8010.47` 在真实二进制下**仍
> 复现**（stale 86.6%、每次 seek 触发一次 `ApplyResolutionChange`）；其相关源码
> 与 `153.0.8010.36` 逐字节相同。

先确认确实在用硬解：`chrome://media-internals` 里 decoder 为 `VaapiVideoDecoder`，
`chrome://gpu` 里 “Video Decode: Hardware accelerated”。

### A/B 对照

同一个 153 二进制，加 `--disable-features=ExtendedVideoBitstreamValidation` 再跑一遍；
再（可选）用 Chrome 152 跑第三遍。分别记录 `stale_repeats`。

### Intel 上可用的日志证据（可选）

```sh
google-chrome-stable --enable-logging=stderr \
  --vmodule=vaapi_video_decoder=3 --user-data-dir=/tmp/chrome-log ... 2>/tmp/chrome.log
grep -c 'ApplyResolutionChange()' /tmp/chrome.log
```

- feature 开：24s/5 次 seek 的运行里约 **6 次** `ApplyResolutionChange()`（启动 1 + 每次 seek 1）。
- feature 关：约 **1 次**（仅启动）。

---

## 5. 结果记录表

2026-09-15 实测：

| 平台 | 会话 | 素材 | feature 开 stale | feature 关 stale | `ApplyResolutionChange` 开/关 |
|---|---|---|---|---|---|
| SM8150/Adreno640（nabu，iris-vaapi） | Wayland | 4K Jellyfin | 74–87% | **0%** | 6 / 1 |
| SM8150/Adreno640（nabu） | Wayland | 1080p 闭GOP 转码 | **88.0%** (settle=15) | **0%** | — |
| Intel CometLake-H UHD（iHD 26.3.2） | X11 | 4K Jellyfin | 6.1% | 5.0% | **8 / 1** |
| Intel CometLake-H UHD | X11 | 1080p 闭GOP 转码 | 9.6% | 7.7% | **14 / 2** |
| Intel CometLake-H UHD | Wayland(GNOME) | 4K Jellyfin | 6.8–8.2% | 6.7–9.1% | — |
| Intel CometLake-H UHD | Wayland(GNOME) | 1080p 闭GOP 转码 | 12.2% | 8.0% | — |

结论（重要）：
- **缺陷平台无关**：两平台 feature 开时每次 seek 都触发 `ApplyResolutionChange`
  （Intel 上 8/1、14/2 也证实了）。
- **可见旧帧症状目前只在 nabu 稳定复现**（Qualcomm iris-vaapi 的 stable surface
  复用 + Wayland/ANGLE）。Intel 无论 X11 还是 Wayland(GNOME)，on/off 无分离
  （底噪 ~7–9%）；把 `analyze.py` 的 settle 从 5 提到 15 也不改变结论（之前
  Intel 单次 18.8% 是 settle 污染）。即：症状还需要 nabu 那条显示/帧池路径。

（nabu 的 Chrome 152 为 0%，H.264 硬/软解均 0%。）

### 关于 settle（底噪）

`analyze.py` 现支持第二个参数指定 seek 后跳过的采样数：
`analyze.py <log> [settle]`（默认 5）。Intel 上 seek 稳定更慢，底噪偏高；
但提到 15 仍无 on/off 分离，所以 Intel 的底噪不是症状。

### Intel 实测注意

- 这台 Intel 上 Chrome 默认拿不到 render node（`vaapi_wrapper.cc GetHandle()
  ... failed to find a suitable render node`），HEVC 直接不支持。加
  `--render-node-override=/dev/dri/renderD128` 后才能用 VA-API/HEVC。`mpv
  --hwdec=vaapi` 正常。
- 转码素材要用**闭 GOP + 每 IRAP 重复参数集**：
  `ffmpeg -i 4K.mp4 -vf scale=1920:1080 -c:v libx265 -preset ultrafast \
   -x265-params keyint=60:min-keyint=60:open-gop=0:repeat-headers=1 -an out.mp4`
  默认 open-GOP(CRA) 的转码在 seek 后会卡死（`ct` 不前进），不能用来判断。

---

## 6. 其它平台

- **Windows（D3D11）**：硬解 HEVC 走同一个 `H265Decoder`
  （`media/gpu/windows/d3d11_video_decoder.cc`），理论同样受影响。用 Chrome 153
  放 HEVC 拖动进度条，再对比 `--disable-features=ExtendedVideoBitstreamValidation`。
- **macOS（VideoToolbox）**：同样基于 `H265Decoder`，但未验证显示层是否也出现旧帧。
- **V4L2 stateful / 其它**：不走这段逻辑，不在范围内。

---

## 7. 注意事项

- Linux Chromium **没有 HEVC 软解**，`--disable-accelerated-video-decode` 会直接无法播放；
  不能靠软解做对照。
- 素材要用 **HEVC**；H.264 本来就是 0%，不能用来判断。
- **先确认启动器没有注入 flag**：`~/.local/bin/google-chrome-stable` 这类 wrapper
  会追加 `--disable-features=ExtendedVideoBitstreamValidation`，使“默认”运行也关
  掉了 feature；复现与 A/B 用真实二进制 `/opt/google/chrome/google-chrome`。
- 统计只看 `stale_repeats`；seek 后的“正常回退一两帧”属 settle，`analyze.py` 已跳过每段前 5 个采样。
- `/tmp` 可能被系统清理，重要日志/截图请存到仓库外的持久目录。

---

## 8. 检查清单

- [ ] `vainfo` 有 `VAProfileHEVCMain` / `Main10`
- [ ] `chrome://gpu` Video Decode = Hardware accelerated
- [ ] `chrome://media-internals` decoder = `VaapiVideoDecoder`
- [ ] 用真实二进制（`/opt/google/chrome/google-chrome`），而不是会注入
      `--disable-features=ExtendedVideoBitstreamValidation` 的 wrapper
- [ ] Chrome 153 默认：`stale_repeats` 高（预期 70–90%）
- [ ] Chrome 153 + `--disable-features=ExtendedVideoBitstreamValidation`：降到 ~0%
- [ ] （可选）Chrome 152：~0%
- [ ] （可选）`ApplyResolutionChange` 次数：开≈每次 seek 一次，关≈仅启动一次
