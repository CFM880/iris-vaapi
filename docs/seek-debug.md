# 4K HEVC seek 残留帧调试记录

本文记录 Xiaomi Pad 5（SM8150）上，Chrome 播放 4K HEVC MP4 时 **seek 后画面回到
seek 前旧帧（残留帧）** 的完整排查过程、自动化测量方法、各实验的结论，以及最终
判断。结论是：该问题无法在 `iris-vaapi`（VA-API 用户态）或 `nabu-iris`（内核）
单方面修复，根因在 Chromium 的 seek/帧池行为。

## 1. 现象

- 素材：4K HEVC 8bit 60Mbps，`Test Jellyfin 4K HEVC 8bit 60M.mp4`。
- 操作：在 Chrome 中播放并拖动进度条（seek）。
- 表现：seek 后画面继续向前播放，但会**周期性跳回 seek 之前的旧画面**；严重时
  停在很旧的帧上。`chrome://media-internals` 中解码器为 `VaapiVideoDecoder`，
  `droppedVideoFrames` 在 seek 后持续增长。
- seek 前正常：显示帧序号严格递增，丢帧稳定。

## 2. 环境与关键事实

- 内核：`6.14.11-nabu1`（`nabu-iris` overlay），`qcom_iris cached_capture=1`。
- 用户态：`iris-vaapi` 的 `vpu_drv_video.so`（`LIBVA_DRIVER_PATH=build`）。
- Chrome 153（Wayland）。
- 内核日志显示：**每次 seek Chrome 都会销毁并重建 VA context**
  （`session_stop`→`session_start`、`created/destroying context`）。

驱动架构要点（见 `docs/architecture.md`）：

- surface 属于 display 级注册表（`src/decode.c` 的 `struct vpu_surfaces`），
  **会跨 VA context 生命周期存活**，backing 是稳定的 DMA-BUF。
- 每个 VA context 有自己的 `vpu_decode_ctx` 和一个 V4L2 stateful session。

## 3. 复现与测量工具（seek-harness）

为了不靠肉眼判断，做了一套自动测量：

1. 本地 HTTP 服务器提供测试页和视频，视频支持 `Range`。
2. 页面用 `<video>` 播放，**每隔若干秒自动 seek** 到设定时间点。
3. 页面定时把「当前帧」的特征上报：
   - 方案 A：驱动在解码帧左上角画**单调递增帧号**，页面用 canvas 读回
     （`VPU_FRAME_STAMP=1`）。
   - 方案 B：页面把画面降采样成 48×27 灰度图并做 FNV 哈希上报，用于检测
     seek 后是否重复出现旧画面。
4. 分析脚本统计「seek 后哈希与之前重复的比例」「丢帧 df 增长」。

脚本见附录 A，放在 `benchmarks/seek_harness/`。

> 注意：早期用**单线程** HTTP server 时，视频 seek 会并发请求多个 Range 块，
> 串行化后造成缓冲停顿，污染了结论（曾误判为「涂黑导致 20s 冻结」）。改用
> `ThreadingTCPServer` 后现象一致，确认不是服务器问题。

## 4. 实验记录

### 4.1 内核/用户态日志定位（Vulkan 拷贝降级）

`journalctl` 抓到：

```
vpu-vaapi: Vulkan DMA-BUF copy failed (不支持的操作); using CPU copy
```

`-ENOTSUP` 来自 `src/vk_copy.c` 的 `import_buffer()` 失败（缓存上限
`VPU_VK_MAX_BUFFERS=192`）。根因：`vpu_decode_destroy()` 没有调用
`forget_vk_capture_buffers()`，每个被销毁的 context 都会把 CAPTURE 源导入
泄漏进 display 级共享 Vulkan 缓存；Chrome 每次 seek 重建 context，约
10 次后缓存占满，之后永久退回 CPU 拷贝。

**修复（保留）**：`vpu_decode_destroy()` 释放该 context 的 Vulkan 源缓冲；
`reset_decoder_session_state()` 清 `vk_copy_failed`。实测 `vk-copy=12 fallback=0`。

该问题解决后 seek 性能改善，但**残留帧依旧**，说明残留另有根因。

### 4.2 帧号叠加与 `present` 日志

在驱动里给每帧画序号（`VPU_FRAME_STAMP=1`，仅诊断），并在
`vpu_surfaces_sync()` 打印 `present id/serial/decoded/queued/epoch`。
实测：seek 后显示的序号在**seek 前的区间内反复**（如 227–245），偶尔跳到新值；
`df` 持续增长。

`present` 日志还显示：建池时 Chrome 会 sync 一批 `serial=0` 的空 surface；seek
后出现 `present id=36 serial=235 decoded=0 queued=0` —— **驱动对「没在当前
session 解过、但 backing 有旧像素」的 surface 直接返回成功**，把旧内容暴露给客户端。

### 4.3 Chrome flag 矩阵

用 harness 对比（34s、7 次 seek；基线无 seek 自身重复率 29%）：

| 配置 | seek 后重复率 | 丢帧 df | 硬解 |
|---|---|---|---|
| `AccelGL+ZeroCopyGL` + ANGLE/GL + 关 Vulkan | 78.4% | 130 | 正常 |
| `VaapiVideoDecoder` + ANGLE/GL + 关 Vulkan | 78.7% | 124 | 正常 |
| 默认 GL + `VaapiVideoDecoder` | 74.4% | 81 | 正常 |
| 开 Vulkan + ANGLE/GL | 82.2% | 79 | 正常 |
| 关 `ZeroCopyGL` | 100% | 0 | 坏（软解/停住） |
| 关 `AcceleratedVideoDecodeLinuxGL` | — | 0 | 坏 |

结论：**换 flag 不改变残留行为**，不是 GL/零拷贝/Vulkan 路径问题。

### 4.4 DBG 日志：context churn 与 assign STALE

`VPU_VAAPI_DEBUG=1` 显示：seek 时大量 `created/destroying context`（Chrome 重建
context），以及大量 `[assign] STALE id=.. generation=N current=M`（旧 generation
的 CAPTURE 被丢弃）。边界逻辑 `stream boundary (idle)` 只在少数 seek 触发。

### 4.5 跨 session 持久 fence（失败）

思路：旧 context 关闭前给其解过的每个 surface 挂一个**跨 session** 的写 fence，
让合成器等新帧；新 context 解码进该 surface 时再 signal。

实现（已回退）：

- 内核 `iris_vidc.c`：新增 `IRIS_SURFACE_FENCE_ATTACH/SIGNAL_PERSISTENT`，fence
  记在模块全局表，`iris_close()` 不再解除；1s 看门狗兜底。
- 用户态 `surface_fence.h`/平台层：新增对应 ioctl 与
  `attach/signal_persistent_surface_fence`；`vpu_decode_destroy`/边界时挂 fence，
  `assign_frame` 时 signal。

验证：独立 `fence_test` 直接调 ioctl，`attach`/`signal` 均返回 0（接口可用）。

结果：**残留重复率不变**（77.1% vs 78.4%），仅丢帧略有下降。→
Chrome/ANGLE 对**已导入**的 surface 不再检查 reservation fence。

### 4.6 同步涂黑失效（失败）

思路：既然合成器不等 fence，就在废弃 surface 时直接写中性黑，保证 backing 里
没有旧像素。

结果：残留消失，但**画面卡在一个固定帧**（黑或最后一帧）；页面 `tf` 显示解码
仍在 60fps 推进，`ct` 也前进，但显示不再切换到新帧。多线程服务器下现象一致。
→ Chrome 合成器在 backing 被改写后**不会刷新**已导入的 EGLImage 对应的显示。

## 5. 根因结论

综合以上：

1. Chrome 每次 seek **销毁并重建 VA context**，但继续**复用 display 级 surface
   池**；旧 surface backing 仍含 seek 前像素。
2. Chrome **不等待**驱动后加的 DMA-BUF reservation fence（对已导入 surface）。
3. Chrome 在 backing 内容被改写后**不刷新**合成（既看不到新帧，又可能显示旧像素）。
4. 因此：
   - fence 方案无法阻止合成器采样旧像素；
   - 涂黑方案会让显示卡死/黑屏。

**内核/驱动无法单方面修复**；正确做法是 Chromium 在 seek（`Reset()`）时
失效/重建 frame pool，使旧 surface 不再被复用。

## 6. 代码状态

保留（已验证）：

- 用户态 seek 边界重构：第一个随机访问帧 + `eos`/`new-sequence`/`idle` 三信号触发
  `stream_boundary_restart()`（`src/decode.c`，`VPU_STREAM_IDLE_MS` 可调）。
- Vulkan 拷贝泄漏修复（见 4.1）。
- 内核移除基于输入时间戳的 seek 过滤（`nabu-iris`，时间戳由用户态合成，本就不
  可用；输出过滤交给用户态边界）。
- H.264/HEVC 的 `new_sequence`（SPS 变化）检测。

已回退：

- 跨 session 持久 fence（内核 + 用户态）。
- 同步涂黑。
- 所有临时诊断（帧号叠加、`present`/`stale sync`/无条件 boundary 日志）。

## 7. 后续方向（需 Chromium 改动）

- `media/gpu/vaapi/vaapi_video_decoder.cc`：`Reset()`/seek 时销毁并重建
  decoder（或显式失效 frame pool / 重新导入 surface），不要复用旧的
  `VASurfaceID` 池。
- 或让合成路径在 surface backing 变化时重新导入 EGLImage。

## 附录 A：harness 脚本

`benchmarks/seek_harness/`：

- `seq_server.py`：多线程 HTTP 服务，`/` 返回测试页，`/video.mp4` 支持 Range，
  `/seq` 收集页面上报。视频路径用 `SEEK_VIDEO` 环境变量指定。
- `seek_test.html`：自动 seek + 画面哈希上报。
- `analyze.py`：统计 seek 后重复率、丢帧。

运行（示意）：

```sh
SEEK_VIDEO=/path/to/test.mp4 python3 benchmarks/seek_harness/seq_server.py &
LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH=$PWD/build \
  google-chrome-stable --user-data-dir=/tmp/chrome-seek --start-fullscreen \
  http://127.0.0.1:8756/
python3 benchmarks/seek_harness/analyze.py /tmp/opencode/seq.log
```

## 附录 B：关键复现命令

```sh
# 模块重载
sudo modprobe -r qcom_iris && sudo modprobe qcom_iris
# 诊断开关
export VPU_VAAPI_DEBUG=1        # 每帧 begin/end/assign 日志
export VPU_FRAME_STAMP=1        # 帧号叠加 + present 日志（诊断用，已回退）
export VPU_STREAM_IDLE_MS=50    # 收紧 seek 空闲阈值
```
