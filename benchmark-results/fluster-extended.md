# 扩展解码测试 — 2026-09-08

使用最终临时加载的 Iris 内核和修复后的 VA-API，FFmpeg Iris 8.0.1 与匹配动态库。
原五个冒烟向量没有修改；本次额外选择七个上游向量，不改参考 MD5。

## 新增 Fluster 向量

| 向量 | 实际码流尺寸 | 软件解码帧数 | 软件 | V4L2 | VA-API |
|---|---|---:|---|---|---|
| cabac_mot_frm0_full | 720×480 | 30 | 通过 | 通过 | 通过 |
| HCMP1_HHI_A | 352×288 | 250 | 通过 | 通过 | 通过 |
| TILES_A_Cisco_2 | 1920×1080 | 100 | 通过 | 通过 | 通过 |
| WPP_A_ericsson_MAIN10_2 | 416×240 | 48 | 通过 | 通过 | 通过 |
| PICSIZE_A_Bossen_1 | 1056×8440 | 10 | 通过 | 失败 | 错误 |
| vp90-2-02-size-lf-1920x1080.webm | 1920×1080 | 217 | 通过 | 通过 | 通过 |
| vp90-2-21-resize_inter_1280x720_5_1-2.webm | 1280×720 → 640×360 | 30 | 失败 | 失败 | 失败 |

软件 6/7，V4L2 5/7，VA-API 5/7。脚本按原参考值判定并返回非零。
PICSIZE 高度 8440 超过驱动最大高度 8192；内核记录 current session not
supported(-22)，不是正常能力范围内的解码通过项。
VP9 变尺寸样本前 10 帧为 1280×720、后 20 帧为 640×360。
其上游参考 MD5 为 dac2d22fc158e751de72918b8755ac1d，但软件输出
36131d7a117ea6c6cec83a589ca9e950，VA-API 输出
b3dc6e42a578c7df1364610dd8a7a412，V4L2 输出
f3ad9995cf2559f19660514c262c4c58。三者不同。后续诊断见下文；原始结果保留，不追改通过状态。

## 本机 4K 视频补测

均为 3840×2160，仅比较前 120 个输出帧；逐帧 MD5 与软件解码对照，
比较输出顺序及像素，不比较时间戳。不是完整视频或长期稳定性测试。

| 文件 | 编码 | V4L2 | VA-API |
|---|---|---|---|
| TheaterSquare_3840x2160.mp4 | H.264 High | 120/120 匹配 | 120/120 匹配 |
| TSU_3840x2160.mp4 | HEVC Main | 不匹配 | 120/120 匹配 |
| Test Jellyfin 4K HEVC 10bit 60M.mp4 | HEVC Main10 | 120/120 匹配 | 120/120 匹配 |
| UshaikaRiverEmb_3840x2160.webm | VP9 Profile 0 | 120/120 匹配 | 120/120 匹配 |
| The World in HDR.mkv | VP9 Profile 2 | 120/120 匹配 | 120/120 匹配 |

所有命令退出 0 且输出 120 帧，但 TSU 的 V4L2 从第 2 个输出帧开始与
软件不同，120 帧的哈希集合也不完全相同，尚不能认定只是顺序变化。
其时间戳也异常；后续隔离实验已将差异定位到容器时间线/丢弃标记处理，见下文。VA-API 同一输入逐帧匹配。
4K 原始文件来自本机 Downloads，并非 Fluster 上游符合性向量。

## 复现与产物

```sh
PATH="$HOME/.local/opt/ffmpeg-iris-8.0.1/bin:$PATH" \
LD_LIBRARY_PATH="$HOME/.local/opt/ffmpeg-iris-8.0.1/lib" \
python3 benchmarks/run_fluster.py /tmp/iris-fluster --suite extended --no-download
```

首次运行去掉 --no-download。固定选取本次七个向量，不代替完整套件。

- fluster-extended.json / fluster-extended-metadata.json：原始 Fluster 结果和环境。
- extended-4k.json：4K 逐项命令、帧数、比较结果与二进制 SHA256。
- logs/fluster/20260908T084920.557322Z/：下载校验、详细命令和日志。
- logs/extended-4k/：逐帧 MD5、stderr 和本机运行脚本 run.py。

本轮只增加测试覆盖和记录结果，没有为消除新失败继续修改解码实现。

## 后续定位：容器时间线与动态画面尺寸

### HEVC TSU：排除本次像素解码差异

MP4 开头四个包带有 AV_PKT_FLAG_DISCARD 且时间戳为负：
-79997、-59998、-39999、-19999；这是 edit list 相关的解码预滚输入。
本机 FFmpeg 的 libavcodec/v4l2_buffers.c 只传递关键帧标记，没有传递
DISCARD。原 V4L2 输出时间戳还出现了 257697 等异常值。

两条路径均在输入前加 `-ignore_editlist 1`，其余维持原 120 帧、
`-fps_mode passthrough -vf format=yuv420p -f framemd5`，软件和 V4L2
120/120 逐帧像素 MD5 完全相同。因此当前证据支持 MP4 时间线/丢弃帧处理
问题，不支持将原差异归因于这些像素解码错误；时间戳异常的具体转换环节
尚未修复。忽略 edit list 改变了播放语义，仅作为诊断，不算原测试已通过。
原始帧哈希：logs/extended-4k/hevc-ignore-editlist-{software,v4l2}.framemd5。

### VP9 resize：软件自动缩放与有效画面尺寸

对同一原始 Fluster 命令增加 `-noautoscale` 后，软件 MD5 为
`dac2d22fc158e751de72918b8755ac1d`，与上游参考完全匹配。原软件失败
来自 FFmpeg 将变化后的 640×360 自动缩放回初始的 1280×720。
上游固定版本和原 Fluster 结果未修改；修正输出策略的诊断另存。

关闭自动缩放后，V4L2 仍输出 30 个 1280×720 帧，总计 41472000 字节；
软件为 10 个 1280×720 + 20 个 640×360，总计 20736000 字节。
将 V4L2 后 20 帧按原 1280/640 平面 stride 取左上角的 640×360 YUV420
有效区域，前 10 帧保持原样，30/30 帧均与软件逐字节相同。
这确认本次 V4L2 像素内容正确，但变尺寸后的可见区域没有正确体现到前端。
HFI FILL_BUFFER_DONE 含 frame_width/frame_height，而当前处理函数没有读取
这两个字段；这是后续驱动调查点，尚未采集固件逐帧字段来证明根因。

同样关闭自动缩放后，VA-API 仍为 29 帧，日志有 EndPicture 和 DQBUF 错误；
尺寸切换流程仍未修复。不能把 V4L2 的手工裁剪成功当成 VA-API 通过。

诊断命令、日志与比较结果：
- logs/extended-4k/vp9-resize-noautoscale.json
- logs/extended-4k/vp9-resize-noautoscale-{software,v4l2,vaapi}.log
- logs/extended-4k/vp9-resize-crop-probe.json

### VA-API context lifetime investigation

`logs/extended-4k/vp9-resize-debug.log` shows a new 640x360 VAContext
at the first resized inter picture (`au=10425`, `started=0`). Its first
submission waits three seconds for sequence discovery, then fails. FFmpeg
subsequently creates another 1280x720 context and emits only 29 frames.
`vpu_vaDestroyContext` destroys the V4L2 session, including firmware reference
state; `vpu_vaCreateContext` always creates a fresh engine. VP9 picture
parameters (including reference surface IDs) are currently ignored by the
codec adapter. A safe continuation mechanism must follow explicit reference
surface ownership; blindly reusing the most recent session risks mixing
independent streams. No context-lifetime implementation change made yet.

A new diagnostic kernel candidate logs FILL_BUFFER_DONE width/height/origin
when VP9 output differs from the current crop. It compiled successfully and
awaits user reload, since sudo requires interactive authentication. This
build only adds rate-limited logging; no dimension behavior is changed.

### Geometry diagnostic confirmed; sufficient-sequence candidate

After loading the diagnostic candidate, the original resize vector produced
HFI output frame=640x360 origin=0,0 while inst crop remained 1280x720 and
CAPTURE remained 1280x736. See vp9-resize-geometry-kernel.log in the same logs
directory. This confirms firmware reports the actual dimensions per frame.

Local upstream Venus vdec.c enables
HFI_PROPERTY_PARAM_VDEC_ENABLE_SUFFICIENT_SEQCHANGE_EVENT (0x100300b),
with a firmware revision gate, to obtain VP9 sequence changes even when
existing buffers suffice. Iris did not set this property. The next candidate
adds its packet encoding and enables it for legacy VP9 during OUTPUT setup.
It must be tested with this VIDEO.IR firmware; support and correct transitions
are not yet established. No synthetic host-side DRC or stride changes added.

### Sufficient-sequence experiment failed before decoding

After user loaded the property candidate, the test returned zero frames
(empty MD5), with an input-setup timeout and vb2 cleanup warnings. FFmpeg
exited zero despite producing no frames; this is a failed test. The state
transition caller resolves to iris_hfi_gen1_sys_event_notify, but there was
no SYS_ERROR/SFR diagnostic. Inspection found this handler unconditionally
reset the core for any event whose session lookup failed, even when the
event was not HFI_EVENT_SYS_ERROR. Thus the log proves a host-triggered
global recovery, not that firmware itself fatally asserted. The original
event payload was not logged, so property support remains undetermined.

The property addition has been reverted. A replacement build also guards
global recovery with HFI_EVENT_SYS_ERROR and logs/ignores other unknown-
session events. It awaits user reload and regression; do not treat this
as resolving VP9 dynamic dimensions. Logs: vp9-resize-seqchange.log and
vp9-resize-seqchange-kernel.log under logs/extended-4k/.

### Follow-up: VA-API resize fix validated

See [VP9 context continuation](vp9-context-continuation.md) for the implemented
reference-owner handoff and rectangular surface copy. With `-noautoscale`,
the original resized vector now produces 30 frames and matches the upstream
MD5 on VA-API in three consecutive runs. Earlier results above are historical;
direct V4L2 visible-size propagation remains unresolved.
