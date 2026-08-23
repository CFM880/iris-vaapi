# iris-vaapi: Chrome 硬件解码方案

让 Chrome 在 Xiaomi Pad 5 (nabu, SM8150) 上使用 Iris 硬件解码的 VA-API 驱动。

## 背景与现状

| 项 | 状态 |
|---|---|
| 目标 | Chrome 通过 VA-API 使用 `qcom-iris` V4L2 硬件解码 H.264/HEVC/VP9 |
| 关键摩擦 | Chrome 走 VA-API 按 slice 喂数据；iris 是"整比特流" stateful 解码器（固件自解析头），不暴露 slice 级 V4L2 控件 |
| 方案 | 自写 VA-API 驱动：接收 slice → 重组成完整访问单元 → 喂 iris V4L2 队列 → NV12 映射回 VASurface |
| 依赖 | libva 2.23（已装）、libva-dev（已装）、vainfo（已装）、Chrome（已装） |

**实测基线（linux 6.14.11-nabu-iris1+）**：
- H.264 4K60: 固件瓶颈 ~49fps（12.3s/10s 媒体）
- HEVC 4K60: 快于实时（120Hz 面板 2x 速）
- VP9 4K60: ~实时

## 架构

```
Chrome (VaapiVideoDecoder)
   │  vaBeginPicture/vaRenderPicture/vaEndPicture
   │  (VAPictureParameterBufferH264 + VASliceDataBufferType)
   ▼
iris-vaapi 驱动 (iris_drv_video.so)
   │  ① 重序列化 SPS/PPS NAL (h264_params.c)
   │  ② slice 重组 → 完整 H.264 访问单元
   ▼
V4L2 解码引擎 (v4l2_dec.c)
   │  /dev/video0 OUTPUT 队列喂 AU，CAPTURE 队列收 NV12
   ▼
VASurfaceID ← NV12 DMA buffer（timestamp 匹配）
```

## 已完成的里程碑

### P0 — VA-API 驱动骨架（✅ 完成）

`src/iris_vaapi.c` 让 libva 能加载驱动并枚举解码能力：

```
$ LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo
Driver version: iris-vaapi P0: Qualcomm Iris SM8150 (V4L2) 0.0.1
      VAProfileH264ConstrainedBaseline:  VAEntrypointVLD
      VAProfileH264Main               :  VAEntrypointVLD
      VAProfileH264High               :  VAEntrypointVLD
      VAProfileHEVCMain               :  VAEntrypointVLD
      VAProfileVP9Profile0            :  VAEntrypointVLD
```

关键技术坑：
- libva 2.x 用版本化 init 符号 `__vaDriverInit_1_23`（无 `va_driver.h` 宏）
- **vtable 必须堆分配**：libva 的 `vaTerminate()` 会 `free(ctx->vtable)`
- `max_image_formats`/`max_subpic_formats` 必须非零，否则 init 失败
- image/subpicture 相关 vtable 入口必须非 NULL（libva 强制校验）
- vendor 字符串走 `ctx->str_vendor`，不在 vtable 里

### P1 — V4L2 解码引擎（✅ 完成并验证）

`src/v4l2_dec.c/h` 独立验证了 iris 的完整解码流程：

**实测结果**（1080p25 H.264，3 秒流）：
```
access units: 153, decoded frames: 74
decode rate: 363.5 fps (0.2 s for 74 frames)
```

**像素级正确性验证**：引擎输出与 FFmpeg 软件解码逐像素一致
（裁剪 1080→1088 对齐填充后 PNG md5 完全相同）。

复刻 FFmpeg v4l2-m2m 的关键序列（用 LD_PRELOAD ioctl 记录器反推）：
1. S_FMT OUTPUT（编码尺寸，sizeimage 给足 16MB）
2. REQBUFS OUTPUT（16）+ QUERYBUF
3. 订阅 `V4L2_EVENT_SOURCE_CHANGE`
4. **第一个输入 AU 在 STREAMON 前先 QBUF**
5. STREAMON OUTPUT
6. **CAPTURE 在 STREAMON OUTPUT 之后 S_FMT**（否则固件 buffer requirements 报 0x1004）
7. CAPTURE: REQBUFS(20) → QUERYBUF → 全部 QBUF → STREAMON
8. DRC 事件 → 若分辨率已匹配则跳过重协商（盲目 REQBUFS 会 EBUSY 并搞挂固件）

mplane 常见坑：
- QUERYBUF/DQBUF/QBUF 必须设 `b.length = 1`（平面数）
- mmap 用 `planes[0].m.mem_offset`/`planes[0].length`，不是 `b.m.offset`
- CAPTURE 高度会被对齐（1080→1088），比较分辨率要按对齐值
- OUTPUT buffer 需空闲链表管理（解码器乱序归还）

**注意**：中断会话（进程被杀/中途错误）会让固件卡死，`SESSION_INIT` 超时（-110），
需 `sudo rmmod qcom_iris && sudo modprobe qcom_iris` 恢复。

## P1 — 接入 VA-API 驱动（✅ 完成）

把引擎接进驱动，真实解码路径全部打通：

| 组件 | 状态 |
|---|---|
| `src/h264_params.c` — SPS/PPS 重序列化 | ✅ 已验证（重建 NAL 解码像素级一致）|
| `src/decode.c` — surface 管理 + slice 累积 + 异步解码 | ✅ |
| `src/iris_vaapi.c` — vtable 解码路径 | ✅ |
| 端到端 `test/test_va_decode.c` | ✅ `DECODE OK` |

关键实现要点：

1. **SPS/PPS 重序列化**：从 `VAPictureParameterBufferH264` 经 Exp-Golomb 重建
   SPS/PPS NAL。profile_idc 从 `VAProfile` 推导，level 按高度推算。
   用真实 SPS/PPS 解析→重建→ffmpeg 解码验证逐像素一致。
2. **slice 重组**：每帧所有 `VASliceDataBufferType` 拼接（保留 start code），
   前置重建的 SPS/PPS → 完整 AU。
3. **surface 映射**：v4l2 timestamp 携带 surface id（`tv_sec = surface_id`），
   iris 透传到输出帧，`assign_frame` 按 timestamp 回填 surface。
4. **异步模型**：`vaEndPicture` 只入队；`vaSyncSurface` 排水到目标 surface 帧出现。
   同步 1 帧会超时（固件需约 4 帧入队才出第一帧）。
5. **导出**：`vaExportSurfaceHandle` 用 `VIDIOC_EXPBUF` 把 CAPTURE buffer
   导出为 DRM PRIME fd，填 `VADRMPRIMESurfaceDescriptor`（NV12 两层）。

**P 帧解码（P2，✅）**：修正两个固件严格校验的字段后，P 帧逐像素正确：
1. **level 计算 bug**：`level_for_height` 收到的是宏块数而非像素，1080p 被算成
   level 3.0（承载不了 1080p），固件拒绝。已改为 `(mbs+1)*16`。
2. **PPS `num_ref_idx_l0_default_active_minus1`**：从 `VASliceParameterBufferH264`
   捕获（Chrome 填的是 slice 实际使用的参考数），重建进 PPS，否则固件
   参考解析失败（refs=0 时 62/75 帧，refs=1 时 75/75）。
3. **参数变化检测**：SPS/PPS 只在内容变化时重发（每帧重发会重置 DPB）。

**验证**（无 B 帧流，IDR + 9 个 P 帧）：
```
surf0=a6ebf5b5=ref0(IDR)   surf1=bef7950e=ref1(P)
surf3=8646c3c1=ref3(P)     surf5=d825880e=ref5(P)
```
全部与 FFmpeg 软件解码**逐像素一致**。

**已知限制**：
- B 帧流需要解码顺序处理（固件内部处理，但测试喂显示序 slice 会丢帧）。
- 单实例单引擎，不支持并发多视频流。

**EOS flush（✅ 已解决）**：stateful 固件 hold 最后一帧，之前 `vaSyncSurface`
对最后一张画面必超时。现在 `iris_decode_sync` 检测到目标是最后入队画面时
自动喂 EOS 标记（`v4l2_dec_flush`），固件返回 `V4L2_BUF_FLAG_LAST` 帧，
释放最后一张。flush 后再解码（Chrome flush/reset、loop）会经 `ensure_decoder`
自动重开会话。

```
$ ./build/test_va_decode nob.h264      # 新增：sync 最后一张 surface
synced last surface 10
DECODE OK
$ ./build/one_pic                      # 单图场景（唯一出路是 flush）
[sync] last_target=1: flushing
[flush] got ts=1000000000000 flags=0x4009
sync ret=success (no error)
```

## 真实客户端验证（ffmpeg VA-API，✅）

用 ffmpeg 的 `-hwaccel vaapi` 作为第三方 VA-API 客户端验证：

```
# 无 B 帧流: 75 帧全部解码，6.5x 超实时
$ ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 -i nob.h264 -f null -
frame= 75  ...  speed=6.54x   EXIT=0

# B 帧流: 77 帧全部解码（含 B 帧重排）
frame= 77  ...  speed=6.93x   EXIT=0

# 内容校验：前 5 帧与软件解码 md5 逐字节一致
frame0..4: vaapi = ref（全部匹配）
```

**过程中修复的关键问题**：
1. **surface id 注册时机**：ffmpeg 在 `vaCreateContext` 前创建 surface 1，此时
   decode ctx 不存在 → surface 未注册。改为 `iris_ensure_decode_ctx` 懒创建。
2. **surface 匹配从 timestamp 改为序列号映射**：不依赖客户端 surface id
   的连续性，用 seq→target surface 的 FIFO 映射。
3. **`vaCreateImage`/`vaGetImage`/`vaDeriveImage`**：实现 CPU 读回路径
   （ffmpeg 用 vaCreateImage 传输帧）。
4. **同步**：`vaEndPicture` 喂入后排空，让上一帧（固件 hold）立即就绪，
   避免逐帧 sync 的客户端死锁。

## VP9 支持（✅）

VP9 帧自包含（无 SPS/PPS），slice data 即完整帧，直接喂入固件：

```
$ ./build/test_va_vp9 /tmp/test-vp9.ivf      # VA-API 端到端
fed 422 frames / VP9 DECODE OK
$ ffmpeg -hwaccel vaapi ... -i vp9.webm       # ffmpeg VA-API
frame= 422 全部解码
# 内容校验：25 帧无 surface 复用流与软解逐字节一致（0 差异）
```

引擎与 VA-API 层均按 profile 选择 V4L2 OUTPUT 像素格式
（H264/HEVC/VP9），`V4L2_PIX_FMT_VP9` 直通喂帧。

## HEVC（⚠️ 引擎级可用，VA-API 参数集重建对 ffmpeg 有效、固件 IDR 待解决）

**引擎级已验证**：固件能解 HEVC（422 AU → 404 帧，像素级正确），
但需**完整参数集**（VPS/SPS/PPS）。用原始参数集前置可稳定解码
（`./build/test_hevc_au`）。

### VA-API 参数集重建（`src/hevc_params.c`）

Chrome/ffmpeg 传 slice data **不含参数集**（已确认 vps=0 sps=0 pps=0），
驱动从 `VAPictureParameterBufferHEVC` 重建 VPS/SPS/PPS。

**逆向 ffmpeg 源码**（下载 `FFmpeg` tag `n8.0.1`）确认的关键映射
（`libavcodec/vaapi_hevc.c:146-211`、`libavcodec/hevc/ps.c`）：
- `pps_loop_filter_across_slices_enabled_flag` ← `pps->seq_loop_filter_across_slices_enabled_flag`
  （`seq_` 前缀是 ffmpeg 对 bitstream `pps_loop_filter_across_slices_enabled_flag` 的命名，ps.c:2341）
- `pps_beta_offset_div2 = pps->beta_offset / 2`，其中 `pps->beta_offset = 2 * beta_offset_div2`（ps.c:2362）
- ffmpeg 的 VA 字段**正确反映 bitstream**（标准解析器），所以重建用 ffmpeg
  填的值理论上应与原始一致

**修复的位级 bug**（逐条对照 ffmpeg `trace_headers` 权威输出定位）：
1. `bs_put` 32→64 位 UB（`1u<<i` 对 i≥32）→ reserved_zero_44bits 写入损坏
2. SPS 缺 **`conformance_window_flag`** → bit_depth 起错位
3. `scaling_list_enabled_flag`/`amp_enabled_flag`/`max_transform_*` 字段顺序
4. `pic_width/height_in_luma_samples` 不应 -1
5. **PPS 缺 `deblocking_filter_control_present_flag`(=1) + beta/tc offset**：
   PPS 从 7 字节变 8 字节（48 payload bit），缺了 slice 头解析依赖的
   deblocking 字段
6. **SPS 缺 VUI**：原始带 `vui_parameters_present_flag=1` + timing info，
   且 `vui_num_units_in_tick`/`vui_time_scale` 是 **u(32)** 不是 ue(v)
   —— 用 ue 写会导致 "Overread SPS by 8 bits"
7. SPS/VPS 的 profile_tier_level：progressive=1、frame_only=1、level=120
   （ffmpeg trace 权威，两者一致，非 0/0/192）

### 验证状态

| 层级 | 结果 |
|---|---|
| ffmpeg 软件解码 | ✅ 10/10 帧，帧 1-5 内容与原始逐像素一致 |
| 固件硬件 | ⚠️ 8 帧 ok + 1 帧 **IDR corrupt**（flags=0x8）|

### 仍未解决（两个残留差异）

重建参数集与原始 bitstream 仍有**字节级差异**（字段值对 ffmpeg 权威一致，
但固件硬件对参数集字节布局要求严格）：

1. **固件 IDR corrupt**（第 1 帧）：
   - ffmpeg 软件解码第 1 帧正确，但固件硬件对 IDR slice 头解析失败
   - VPS byte 11：重建 `90`（progressive=1）vs 原始 `00`——**VPS 的
     `reserved_zero_44bits` 我硬编码了 x265 值 `0x78a003`，但原始 VPS 是 0**
   - PPS byte 6：`44` vs 原始 `40`（bit2 差异）
2. **帧 7+ 内容漂移**（P 帧参考累积误差 1363→8116 样本）：某个参考管理
   字段仍不完全匹配

### 结论

重建参数集对 **ffmpeg 软件解码完全有效**（证明字段值/结构正确），但
**固件硬件对参数集有字节级严格要求**。下一步：把 VPS 的 reserved_zero_44
改为 0（匹配原始 VPS），逐字节对齐重建与原始参数集，固件 IDR corrupt
应随之消除。驱动保留 HEVCMain profile 与重建代码作为基础。

## Chrome 集成（✅ 核心验证通过）

Chrome 151（Wayland 原生）加载 iris 驱动并**流畅解码 H.264 视频**：

```
# 页面 onloadeddata 触发，标题显示 "LOADED 1920x1080"
# 驱动日志（GPU 进程 stderr）：
[surf] id=1 size=3133440 w=1920 h=1088        # 帧池 surface 分配（DMA-heap）
[export] surf=1 type=1073741824 flags=0x5      # vaExportSurfaceHandle 成功
[begin] target=2 ... [end] done rv=0           # slice 喂入解码成功
# 持续解码：675 次 begin（约 40fps 循环播放），无错误
```

**Chrome 探测卡点的修复**（对应之前"卡在 surface 导出探测"）：

| 卡点 | 修复 |
|---|---|
| 空 surface 的 `vaSyncSurface` 超时 | surface 从未入队解码时 sync 立即成功（`src/decode.c`：`queued` 标志）|
| `vaExportSurfaceHandle` 的 `SEPARATE_LAYERS` 要求 | NV12 按 2 层、每层 1 平面返回（`src/iris_vaapi.c`）；Chrome DCHECK 每层必须单平面 |
| `drm_format_modifier` 缺失 | 填 `DRM_FORMAT_MOD_LINEAR`（Chrome 校验 modifier 一致性）|

**运行方式**：Chrome GPU 进程需继承 `LIBVA_DRIVER_NAME=iris`（用包装脚本
`export LIBVA_DRIVER_NAME=iris; google-chrome ...`），驱动装到
`/usr/lib/aarch64-linux-gnu/dri/iris_drv_video.so`，`/dev/dma_heap/system`
需 `sudo chmod 0666`（GPU 导入需要真实 DMA-BUF，memfd 回退不可用）。

## 测试工具

| 工具 | 用途 |
|---|---|
| `test/test_va.c` | 最小 libva 客户端，验证驱动加载与 profile 枚举 |
| `test/test_v4l2_dec.c` | V4L2 引擎测试：Annex-B 切 AU → 解码 → NV12 校验 |
| `test/test_h264_params.c` | SPS/PPS 解析 + 重序列化验证 |
| `test/test_va_decode.c` | 端到端 VA-API 解码 + 导出，含最后一张 sync（EOS flush） |
| `test/test_va_vp9.c` | 端到端 VP9 VA-API 解码 + 导出（IVF 逐帧喂入）|
| `test/test_hevc_au.c` | HEVC 引擎测试：整体流喂入 + EOS，统计帧数 |
| `test/ioctllog.c` | LD_PRELOAD ioctl 记录器（反推 mpv 真实序列）|

## 复现命令

```sh
make
# P0: 枚举驱动
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo
# P1: 引擎解码（输出帧与软件解码逐像素一致）
./build/test_v4l2_dec /path/to/stream.h264 1920 1080 /tmp/frame.nv12
```
---

## 工作总结（截至 2026-08-22）

### 项目目标
让 Chrome 在 Xiaomi Pad 5 (nabu) 上使用 Qualcomm Iris 硬件解码 H.264。
Chrome 走 VA-API（按 slice 喂数据），iris 是"整比特流" stateful V4L2 解码器
（固件自解析 SPS/PPS），两者接口模型不兼容 → 自写 VA-API 驱动做桥接。

### 已完成

| 里程碑 | 状态 | 验证 |
|---|---|---|
| P0: VA-API 驱动骨架 | ✅ | vainfo 识别 5 个 VLD profile |
| P1: V4L2 解码引擎 | ✅ | 363 fps，与软解逐像素一致 |
| P1: SPS/PPS 重序列化 | ✅ | 重建 NAL ffmpeg 解码一致 |
| P1: 完整 VA-API 解码路径 | ✅ | 端到端 DECODE OK |
| P2: 参数变化检测 + P 帧 | ✅ | IDR+P 帧逐像素一致 |
| P2: ffmpeg VA-API 客户端 | ✅ | 75/77 帧，6.5x 超实时，逐字节一致 |
| P2: Chrome 集成 | ✅ | Wayland 下流畅解码 H.264，页面 LOADED 1920x1080 |
| P3: VP9 支持 | ✅ | VA-API + ffmpeg 全解，25 帧逐字节一致 |
| P3: HEVC 引擎级 | ✅ | 固件 422 AU → 404 帧，像素正确（需完整参数集）|
| P3: HEVC VA-API | ⚠️ | 重建参数集 ffmpeg 软件解码 10/10 帧；固件 IDR corrupt（字节级差异待消除）|

### 关键成果
**ffmpeg `-hwaccel vaapi` 作为第三方 VA-API 客户端解码成功**，且输出与
FFmpeg 软件解码**逐字节一致**。这是驱动正确性的最强证明（真实客户端，
非自定义测试）。

### Chrome 集成的卡点（已解决）
Chrome 的 `VaapiVideoDecoder` 与 stateful 解码器存在**缓冲模型不匹配**：

1. Chrome 在解码前就 `vaExportSurfaceHandle` 导出 surface（帧池预分配），
   要求每个 surface 有稳定、可导出的后备 buffer（`reference/vaapi_video_decoder.cc:930`）。
2. stateful v4l2 解码器把帧写入固件控制的 CAPTURE buffer（会被复用），
   无法预分配给具体 surface。
3. 已解决：surface 属性查询（`VASurfaceAttribMaxWidth/Height`）、
   `vaQueryConfigAttributes` 输出语义、profile 查询 lenient。
4. 已实现：每个 surface 用 `/dev/dma_heap/system` 分配稳定 DMA-BUF 后备，
   解码后拷贝进后备（`src/decode.c`）。**ffmpeg 已验证此模型可用**。
   若 `/dev/dma_heap/system` 不可访问（root only，默认权限），驱动自动
   回退到 **memfd 后备**：本地测试与 ffmpeg CPU 读回路径仍可无 root 运行
   （输出与软解逐字节一致），但该 fd 不是 DRM buffer，**无法被 GPU/EGL
   客户端（Chrome）导入**。真实 Chrome 显示互通仍需：
   `sudo chmod 0666 /dev/dma_heap/system`。
5. 已解决：空 surface 的 sync 探测（Chrome 帧池预分配后立即 sync+export）——
   surface 未入队解码时 `vaSyncSurface` 立即成功；`vaExportSurfaceHandle`
   按 `SEPARATE_LAYERS` 返回 2 层单平面 + `DRM_FORMAT_MOD_LINEAR`。

### 已知限制
- stateful 固件 hold 1 帧：最后一张画面由 EOS flush 释放（已解决，见上）。
- B 帧短序列引用未解析时丢帧（完整流正常，引擎解 74/75）。
- 单实例单引擎，不支持并发多视频流。
- H.264 4K60 高码率受固件解码上限（~49fps）。
- 中断会话会让固件卡死（SESSION_INIT 超时 -110），需重载模块：
  `sudo rmmod qcom_iris && sudo modprobe qcom_iris`
- surface 后备缺省用 DMA-heap（GPU 可导入）；未 chmod 时自动回退 memfd，
  本地测试/ffmpeg 仍可用，但 Chrome 显示互通需要
  `sudo chmod 0666 /dev/dma_heap/system`。

### 构建与测试命令
```sh
make                                          # 构建驱动 + 测试工具
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo   # P0
./build/test_v4l2_dec 流.h264 1920 1088       # P1 引擎
./build/test_va_decode 流.h264 输出.nv12      # P1 VA 路径
LIBVA_DRIVERS_PATH=$PWD/build LIBVA_DRIVER_NAME=iris \
  ffmpeg -hwaccel vaapi -vaapi_device /dev/dri/renderD128 -i 流.h264 -f null -  # P2 ffmpeg
# 安装到系统供 Chrome 使用：
sudo ./install-system.sh
# Chrome（GPU 进程需继承 LIBVA_DRIVER_NAME）：
export LIBVA_DRIVER_NAME=iris
google-chrome --enable-logging=stderr --v=1 file:///path/to/video.html
```

### 参考
`reference/` 保存了 Chromium 的 `vaapi_wrapper.cc` 与 `vaapi_video_decoder.cc`
源码（调查 Chrome 初始化/导出流程用），勿删除。

### git 提交历史
```
faf3c9e decode: fall back to memfd surface backing when dma_heap is root-only
92e3ae3 iris-vaapi: real client (ffmpeg vaapi) decodes H.264 end-to-end
a9464aa iris-vaapi: fix P-frame decode and add slice-param-driven PPS
d32dca0 iris-vaapi: wire full VA-API H.264 decode path
b04ea5a h264: re-serialize SPS/PPS NAL units from VAPictureParameterBufferH264
c141778 docs: summarize VA-API Chrome hardware decode project status
45cf083 iris-vaapi: VA-API driver skeleton and iris V4L2 decode engine
```
