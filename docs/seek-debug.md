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

- surface 属于 display 级注册表（`src/decode/surface.c` 的 `struct vpu_surfaces`），
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
  `stream_boundary_restart()`（`src/decode/stream.c`，`VPU_STREAM_IDLE_MS` 可调）。
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
export VPU_FRAME_STAMP=1        # 顶部 16-bit 帧号条码（见第 8 节）
export VPU_STREAM_IDLE_MS=50    # 收紧 seek 空闲阈值
```

---

## 8. 帧号级定位与“驱动侧无解”论证（2026-09-14）

第 1–7 节的结论是“根因在 Chromium 合成器，驱动无法单方面修复”，但当时缺少
**帧级证据**，也留有一个未验证的假设：能不能在驱动侧于呈现时刻拦截 stale
backing。本节用帧号戳把这一点做实，并逐个否证所有驱动侧方案。

### 8.1 新增工具

1. **驱动帧号戳（`VPU_FRAME_STAMP=1`）**
   每个成功解码并写入 backing 的帧，在 luma 平面顶部 `sh/8` 行画一个 16 格
   条码：第 `i` 格覆盖 `[i·sw/16, (i+1)·sw/16)`，bit=1 画 Y=235，bit=0 画
   Y=16。串号来自全局原子计数器，随解码单调递增，**唯一标识驱动解的每一帧**。
   代码位置：`src/decode/surface.c` 的 `surface_stamp()` / `surfaces_mark_decoded()`。

2. **页面读回条码**
   `seek_test.html` 把视频缩放到 48×27 后，在第 1 行按格中心采样 16 个 bit，
   还原串号并随 `sn=` 上报；`analyze.py` 解析 `sn`。

3. **修正 harness 的一个真 bug**
   原页面写的是
   `ctx.drawImage(v, 0,0,W,H, 0,0,W,H)`（9 参数形式），即只取视频**左上角
   48×27 裁剪**再放大，并非降采样。因此第 4 节所有基于 hash 的“重复率”都在
   看一个角落，只能当粗指标。已改为 `ctx.drawImage(v, 0, 0, W, H)`。

4. **验证**：一次运行中页面读回 118 个 `sn`，其中 **116 个与驱动打印的
   `[stamp] serial=N` 精确相等**，其余 2 个是初始未解码帧。条码可信。

### 8.2 关键观测：seek 后合成器被钉死在单一 surface

`VPU_FRAME_STAMP=1`，无改写（`off`），一次 seek 前后的显示串号：

```
pre-seek : 0 4 15 19 23 32 37 44 49 53 60 65 71 76 81 88 ...   (单调递增)
seek     -> 300
post-seek: 237 237 237 237 239 243 236 235 236 247 244 240 241 242 239 247
           235 244 237 224 323 232 244 224 221 243 247 240 239 370 244 240
           389 393 236 403 239 237 239 425 ...
```

现象：
- seek 后显示串号停在旧区间（约 227–250 的一张固定帧），并**在后续每个
  seek 分段里反复出现同一张**（seg2..seg5 仍是 236–248）。
- 期间驱动仍在正常解新帧（323/370/389/403/425… 都是新串号），但只在画面
  上闪现一两帧，随后又回到被钉的那张。
- 驱动日志无任何错误：没有 `readiness wait failed`、没有 `fatal`；
  `vaEndPicture` 对导出 target 的 backpressure 保证 target 已在本 epoch 解出。

`chrome://media-internals` 侧 `tf`（totalVideoFrames）持续增长，说明
Chromium 解码器确实在输出新帧——**是合成器不切换，而不是驱动不解码**。

### 8.3 A/B 实验结果

`VPU_EPOCH_REWRITE` 是本次加的驱动侧实验开关：在 stream epoch 切换时对上一
epoch 的 backing 做不同处理。**该开关已在实验后移除（结果保留如下）**，以免把
无效路径留在数据面。

| 模式 | epoch 切换时的动作 | seek 后串号序列 | 结果 |
|---|---|---|---|
| `off` | 不动作 | `237 237 237 237 239 243 236 … 323 … 370 …` | 反复回退旧帧 |
| `black` | 全部 stale backing 涂黑 | `237 237 237 237 0 0 0 0 …` | **永久黑，不再前进** |
| `last` | 全部 stale backing 填最后一帧 | `237 237 237 237 0 250 250 0 250 …` | **冻结在最后一帧** |
| `mirror` | 每解一帧把新帧拷进被钉 surface | 基本停在 `227`；16s 仅 9 次解码 | 管线停摆，无新帧可 mirror |

其中：
- `black` 精确复现了 4.6 的“涂黑后卡死”：驱动**能**改被钉 backing 的内容
  （串号立刻变 0），但改了之后合成器**永远显示这张**，不再前进。
- `mirror` 本意是“既然钉住了，就让钉住的那张一直跟着最新帧走”。但实测
  16s 内只发生 9 次解码——说明合成器不释放那张 frame，Chromium 的
  `DmabufVideoFramePool` 被占死，解码管线几乎停摆，根本没有新帧可镜像。

> 注：`VPU_EPOCH_REWRITE` 必须通过 `export` 传给启动 shell 才会进入 GPU 进程；
> 早期用 `env VAR=… timeout … chrome` 的写法未生效，导致一组“三模式完全相同”
> 的假数据，已由 `export` 复测纠正。

### 8.4 对照实验：mpv 在同一驱动上不回退

用第三方 stateful VA-API 客户端 mpv 0.41（`--hwdec=vaapi --vo=gpu-next
--gpu-context=wayland`）在同一真机、同一驱动、同一素材上，经 IPC 按 harness
相同的时刻表自动 seek，并从 mpv 自己的截图中读回驱动帧号条码：

```
seg0(播放中)  54 300 737 985
seg1(seek)   1424 1749 1951 1951
seg2(seek)   1958 1958 1958 1958
seg3(seek)   1963 1963 1963 1963
seg4(seek)   1968 1968 1968 1968
seg5(seek)   1976 1976 1976 1976
seg6(seek)   1984 1984 1984 1984
```

- 帧号**单调递增，跨 seek 0 次下降**（段内重复只是同一帧被连续截到）。
- 驱动日志：5 次 `stream boundary`、`[assign] STALE=0`、无
  `readiness wait failed`/`fatal`，确认用的是本驱动
  （`Using hardware decoding (vaapi)`、`VO: [gpu-next] 3840x2160 vaapi[nv12]`、
  1987 次 `[stamp]`）。

结论：**同一个驱动、同样的稳定 surface/backing 模型，mpv 在 seek 后帧号只前进
不回退**。这直接排除“驱动把 stale backing 交给客户端”，把问题锁定在 Chromium
的合成器/frame-pool 行为上，与 8.2 的帧号观测互相印证。

> 说明：mpv 的 `screenshot-to-file` 取的是它自己 VO 渲染的帧，严格说不是
> Wayland 合成器扫描出的画面；但“驱动串号连续 + `STALE=0`”足以证明驱动侧输入
> 正确，Chromium 是唯一变量。

### 8.5 逐条否证驱动侧方案

| 方案 | 为什么无效 |
|---|---|
| `vaSyncSurface` 时回填 stale backing | Chromium 的 Linux VA-API 路径**几乎不调 `vaSyncSurface`**。整场仅 24 次，全部是建池时对新 surface 的探测（`decoded=0 queued=0 init=0`）。解码帧不走 sync。 |
| `vaEndPicture` 加 backpressure | 已有。它只保证**当前 target** 已解出；出问题的是合成器呈现的那张旧 frame，不是 target。 |
| 主动涂黑 stale backing | 见 8.3：改内容有效，但合成器就此停在黑帧。 |
| 主动填最后一帧 | 同上，冻结在最后一帧。 |
| 每帧镜像到被钉 surface | 合成器不释放 frame → pool 占死 → 解码停摆。 |
| 跨 session 持久 reservation fence（4.5） | 合成器/ANGLE 对**已导入**的 surface 不再检查该 fence。 |
| 重新导入 surface | Chromium 按 `VASurfaceID` 缓存 EGLImage，`Reset()` 不失效、不重导；驱动无法迫使它重导。 |

### 8.6 它到底是不是“无解”

**不是物理上无解，而是在“不动 Chromium、不动 player、只改通用 VA 驱动/内核”这
个约束下无解。** 精确地说：

1. 失败发生在 Chromium 的 **frame 选择/释放状态机**里：seek 后合成器持续呈现
   一张旧 `VideoFrame`，且不释放它，同时解码器仍在产出新帧。驱动拿不到
   “当前正在扫描哪张 frame”“让 Chromium 释放/切换”的任何接口。
2. 驱动唯一能影响显示的杠杆是**改 backing 内容**（已被帧号戳证明有效）。但
   内容一改，合成器只是显示新内容并继续停在同一张上——对“旧帧回退”而言，
   黑化和填最后一帧都只是把回退换成冻结（4.6 及 8.3）。
3. 标准隐式同步本可让 GPU/合成器等待，但内核没有“用户态给 dma-heap buffer
   挂标准 `dma_resv` fence”的通用接口；iris 自定义 ioctl fence 不被 Adreno
   读取。
4. 把 V4L2 CAPTURE buffer 直接导出给 Chromium（去掉稳定 backing 拷贝）本可
   吃到标准 V4L2 同步，但 stateful 固件会复用 CAPTURE，无法预导出，且 frame
   选择仍在 Chromium。

**让它可解的条件**（任一条）：
- Chromium 在 `Reset()`/seek 时失效或重建 frame pool、或对已导入 surface 重新
  绑定 backing（`media/gpu/vaapi/vaapi_video_decoder.cc`）；
- 或改走 Chromium 原生 stateful V4L2 解码器
  （`media/gpu/v4l2/v4l2_stateful_video_decoder.cc`，其 `Reset()` 按 V4L2
  spec 做 `STREAMOFF/STREAMON`，seek 语义由 Chromium 自己维护）；
- 或合成/显示路径在 surface backing 变化时重新导入 EGLImage；
- 或内核提供通用的 dma-buf 导出 fence 语义并被 Adreno 合成路径遵守。

在这些之外，继续在驱动里换写法（更早/更晚回填、阻塞 sync、返回错误等）都不会
改变“合成器选择哪张 frame”这一事实。

### 8.7 代码状态与复现

本次改动（`make check` 全绿）：
- **保留**：帧号戳诊断（`VPU_FRAME_STAMP`）、harness 的 `drawImage` 修正、
  `analyze.py` 的 `sn` 统计、epoch 兜底（`surfs_backfill_stale`，在
  `vaSyncSurface` 上对被判为 stale 的 surface 重复当前 epoch 最新帧或涂黑，
  对 Chromium 实际路径不触发，作为安全网保留）。
- **已移除**：实验分支 `VPU_EPOCH_REWRITE=black|last|mirror` 及
  `surfaces_mark_decoded` 里的 mirror 逻辑（8.3 的结论已证明其无效）。

复现（设备为 nabu，Chrome 在已运行的 Wayland 会话里被远程拉起）：

```sh
make
# 帧号戳 + 条码读回
LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH=$PWD/build VPU_FRAME_STAMP=1 \
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  timeout 40 google-chrome-stable --ozone-platform=wayland \
  --user-data-dir=/tmp/chrome-seek --start-fullscreen \
  --enable-features=VaapiVideoDecoder http://127.0.0.1:8756/
python3 benchmarks/seek_harness/analyze.py /tmp/opencode/seq.log
```

