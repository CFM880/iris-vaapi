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

**已知限制**：
- stateful 固件有 1 帧 hold 延迟：最后入队的画面需再喂一帧（或 EOS）才释放，
  对应 `vaSyncSurface` 对最后一张画面会超时。
- 每帧重发 SPS/PPS 会重置 DPB，P 帧参考依赖可能失效；真实 Chrome 只在参数
  变更时发 SPS/PPS（驱动需加"参数变化才重发"逻辑）。
- 单实例单引擎，不支持并发多视频流。

## 待办（P2）

- Chrome 端实际加载驱动解码验证（EGL/Wayland 显示互通）
- 参数变化检测（避免每帧重发 SPS/PPS）
- EOS flush 释放最后一帧
- HEVC/VP9 slice 重组支持

## 测试工具

| 工具 | 用途 |
|---|---|
| `test/test_va.c` | 最小 libva 客户端，验证驱动加载与 profile 枚举 |
| `test/test_v4l2_dec.c` | V4L2 引擎测试：Annex-B 切 AU → 解码 → NV12 校验 |
| `test/test_h264_params.c` | SPS/PPS 解析 + 重序列化验证 |
| `test/ioctllog.c` | LD_PRELOAD ioctl 记录器（反推 mpv 真实序列）|

## 复现命令

```sh
make
# P0: 枚举驱动
LIBVA_DRIVER_NAME=iris LIBVA_DRIVERS_PATH=$PWD/build vainfo
# P1: 引擎解码（输出帧与软件解码逐像素一致）
./build/test_v4l2_dec /path/to/stream.h264 1920 1080 /tmp/frame.nv12
```