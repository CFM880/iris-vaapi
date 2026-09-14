# Fluster 完整套件结果（stock FFmpeg）

2026-09-14 在 nabu 真机运行 Fluster 完整四套（`--suite full`，pinned 版本
`f3ad284a9e6cac70dc01b02e0de71c2994181d34`）。本结果**使用发行版自带的
`/usr/bin/ffmpeg`（8.0.1-3ubuntu2），不使用本项目自编译/打补丁的
`ffmpeg-iris-8.0.1`**，因此反映只安装 `vpu-vaapi` 驱动即可达到的状态，而不是
前端也被改造后的状态。

- 运行：`benchmark-results/logs/fluster/20260914T091541.171377Z`
  （约 12 分钟，exit 1）
- 内核：`6.14.11-nabu1`；项目：`a1821c0`
- 驱动：`vpu_drv_video.so` SHA256 `0d7cf4f388daf4a512335aafc41a8f29e0405fd24b614d55a5b232182b4c796b`
- VA-API：`LIBVA_DRIVER_NAME=vpu`，`LIBVA_DRIVERS_PATH=<repo>/build`

## 1. 结果

`passed/total`，与上游参考 MD5 一致才算通过；`()` 内为该路径的失败结果分布。

| 套件（向量数） | 软件 | V4L2 M2M | VA-API |
|---|---|---|---|
| H.264 JVT-AVC_V1 (135) | 130/135 | 116/135（15 Fail/1 Timeout/3 Error） | 97/135（17 Fail/21 Error） |
| HEVC JCT-VC-HEVC_V1 (147) | 141/147 | 130/147（9 Fail/8 Error） | 130/147（7 Fail/10 Error） |
| VP9 VP9-TEST-VECTORS (305) | 242/305 | 176/305\*（63 Fail/66 Error） | 174/305（58 Fail/73 Error） |
| VP9 VP9-TEST-VECTORS-HIGH (6) | 6/6 | 0/6（5 Fail/1 Error） | 2/6（4 Error） |

\* VP9 V4L2 是**部分结果**：pass 在中途因异常中止，见第 3 节。

## 2. 与 2026-09-13 完整运行（使用自编译 FFmpeg）的差异及原因

对照 `benchmark-results/logs/fluster/20260913T050815.987119Z`
（该次用 `/home/cfm880/.local/opt/ffmpeg-iris-8.0.1` + `LD_LIBRARY_PATH`）：

| 套件 | 本次 V4L2 | 09-13 V4L2 | 本次 VA-API | 09-13 VA-API |
|---|---|---|---|---|
| H.264 | 116 | 77 | 97 | 67 |
| HEVC | 130 | 139 | 130 | 133 |
| VP9 P0 | 176\* | 0 | 174 | 78 |
| VP9 High | 0 | 0 | 2 | 2 |

差异**主要不是代码变更**（本次驱动相对上次只多了默认关闭的帧号戳/epoch 兜底），
而是三类环境/前端原因：

1. **固件会话被前面的失败“楔死”**。
   09-13 的 H.264 pass 出现大量 `Timeout`（VA-API 37 个、V4L2 12 个）。被中断的
   session 会让固件进入不可用状态，于是其后的 VP9 V4L2 pass **305 个向量全部**
   报同一错误：
   ```
   output VIDIOC_REQBUFS failed: Input/output error
   ```
   共 305 次，正是该 pass 的向量数。本次 H.264 只剩 1 个 Timeout、VA-API 0 个
   Timeout，固件没被提前楔死，VP9 V4L2 恢复到 176 个。因此 H.264 两路
   （77→116、67→97）与 VP9 VA-API（78→174）的大部分“提升”，是**从污染中恢复**，
   不是驱动变好。该现象在 `docs/fluster.md` 中已有警告（“中断会话会让固件卡死，
   需重载模块”）。

2. **FFmpeg 前端不同（本次为 stock）**。
   09-13 用项目自编译的 `ffmpeg-iris-8.0.1` 并设了 `LD_LIBRARY_PATH`，对
   HEVC/VP9 的 10-bit CAPTURE（P010/NV12）协商做过修改；本次用发行版 stock
   FFmpeg。前端不同会直接影响 10-bit 与色彩格式相关的用例，也影响对不支持格式
   的处理方式（见第 3 节）。因此 HEVC 硬件两路本次略低（V4L2 139→130、
   VA-API 133→130），**不能与使用自编译 FFmpeg 的旧基准直接比较**；要看真实能力
   应统一用 stock FFmpeg（本表）。

3. **VP9 V4L2 仍被“极小尺寸 + 不支持色彩格式”打断**（见第 3 节）：这是 pass
   内部的中止，不是 0 通过。

## 3. 为什么 VP9 V4L2 是部分结果，以及失败原因分类

VP9 V4L2 的楔死起点：
```
vp90-2-02-size-08x08.webm:
  capture VIDIOC_REQBUFS failed: Device or resource busy
  Error submitting packet to decoder: Cannot allocate memory
```
即 **8×8 这类极小尺寸**让 v4l2m2m 路径的 CAPTURE 分配失败并拖垮会话，随后连锁
EIO（本次共 370 次，全部落在该 pass）。之后在
`vp91-2-04-yuv444.webm`（yuv444p，硬件不支持）处 ffmpeg 退出 69，fluster 抛
`CalledProcessError`，该 pass 在 **176/305** 中止，因此是下界而非真实通过率。

按原因归类（硬件路径）：

- **与软件一致失败 → 码流/参考本身问题，非硬件**：H.264 `FM1_BT_B`、
  `FM1_FT_E`、`FM2_SVA_C`（Error）、`SP1_BT_A`、`sp2_bt_b`（Fail）；HEVC
  `NUT_A_ericsson_5`、`RPS_D_ericsson_6`、`SAODBLK_A/B`、`VPSSPSPPS_A`；
  VP9 软件 63 个 Fail（多为 resize）。
- **场/交错编码工具**：H.264 `*_Field_*`、`*_PAFF_*`、`*_MBaff*`、`CAB*`、
  `CAM*`、`CAP*`、`CV*` 在 V4L2/VA-API 上 Fail/Error。
- **尺寸超出能力**：HEVC `PICSIZE_*`（高 8440 > 8192，`Error`）；VP9
  `tiny(<64)` 每条硬件路径各 64 个直接 Error。
- **不支持的分辨率变化/色度/位深**：VP9 `resize` 每路 62 个、`yuv422/yuv444`
  与 10/12-bit 组合；HEVC Main10 的 `WPP_*`/`WPP_D` 在 V4L2 上 Error（P010
  CAPTURE 协商）。
- **前端（stock FFmpeg）限制**：10-bit CAPTURE 协商相关问题，见
  `docs/fluster.md`。

## 4. 局限与正确复现

- 完整套件**必须每个 pass（或遇到不支持格式/EIO 后）重载 `qcom_iris`** 才有
  干净的硬件数字；本机当前 `sudo` 需要密码，无法在 run 中途自动重载，所以
  VP9 V4L2 仍是下界。
- 建议后续用 stock FFmpeg 跑，并给脚本加“每 decoder 前 `modprobe -r/`”的重载
  步骤；或先跳过 `<64` 尺寸与 4:2:2/4:4:4/12-bit，避免触发楔死。

```sh
make
python3 benchmarks/run_fluster.py --suite full --no-download
# 需要干净结果时，在每个 decoder pass 之间重载模块：
sudo modprobe -r qcom_iris && sudo modprobe qcom_iris
```

- [原始 Fluster JSON](fluster-full.json)
- [版本、环境、二进制哈希与命令](fluster-full-metadata.json)
- [运行与分析指南](../docs/fluster.md)
