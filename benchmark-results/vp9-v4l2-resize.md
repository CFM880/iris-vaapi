# VP9 in-place resize over V4L2 — 2026-09-22

Direct V4L2 (`vp9_v4l2m2m`) now reproduces the software output for the
sufficient-resource VP9 resize vectors. The canonical
`vp90-2-21-resize_inter_1280x720_5_1-2` case is byte-identical over three
consecutive runs.

## Firmware behaviour (VIDEO.IR.1.2, legacy VPU5)

- An in-place VP9 size change that fits the existing CAPTURE allocation does
  **not** raise `HFI_EVENT_SESSION_SEQUENCE_CHANGED`.
- The firmware does **not** implement
  `HFI_PROPERTY_PARAM_VDEC_ENABLE_SUFFICIENT_SEQCHANGE_EVENT` (0x100300b); its
  only sequence property is `SEQCHNG_AT_RAP`. Sending 0x100300b fails the
  session, which is why the earlier property experiment timed out.
- Firmware strings confirm the intended behaviour:
  `Seq changed at INTER frame. Not calling update ds info. We will continue to
  use old values for now.` The OUTPUT2 configuration is kept, so the CAPTURE
  allocation must not change; a reconfiguration `HFI_FLUSH_OUTPUT` during this
  window asserts the firmware (`sys error ... session error 0x1004`, followed
  by the host power-cycle).
- Instead the firmware reports the visible rectangle per frame in
  `FILL_BUFFER_DONE` (`frame_width`, `frame_height`, `start_x_coord`,
  `start_y_coord`).

## Fix

Driver (`nabu-iris`, kernel overlay):

- `iris_hfi_gen1_session_ftb_done()` detects a shrink of the VP9 visible
  rectangle (legacy VPU5, after the first IPSC, not already in DRC) and
  publishes the new rectangle through `inst->crop`; it also queues
  `V4L2_EVENT_SOURCE_CHANGE` without changing the CAPTURE format, so a client
  that re-queries `G_FMT` sees no resolution change and does not tear the
  queue down.
- Each CAPTURE buffer records its own rectangle (`struct iris_buffer.crop`),
  and `iris_vb2_buf_finish()` exposes the dequeued buffer's rectangle through
  `G_SELECTION`. This is required because legacy VP9 output is held one frame
  for superframe handling, so the session-level `crop` can run one frame ahead
  of the buffer being dequeued.

Client (`FFmpeg`, V4L2 m2m wrapper):

- After `VIDIOC_DQBUF`, query `VIDIOC_G_SELECTION` (`V4L2_SEL_TGT_CROP`, plain
  `V4L2_BUF_TYPE_VIDEO_CAPTURE`) and crop the software frame to that rectangle
  without touching the plane strides or backing buffer. This is the "manual
  crop" the V4L2 API expects of a client when only the visible area changes.
  It also lifts the HEVC smoke result from 1/2 to 2/2.

## Results (`benchmark-results/logs/fluster/resources/VP9-TEST-VECTORS`)

`vp90-2-21-resize_inter_*`, software versus `vp9_v4l2m2m`, `-noautoscale`:

| Vector | software | V4L2 |
|---|---|---|
| 1280x720_5_1-2 | `dac2d22f…` | match |
| 1280x720_7_1-2 | `dbe6f8ec…` | match |
| 320x240_5_1-2 | `90c58a41…` | match |
| 320x240_7_1-2 | `1a8e782d…` | match |
| 640x480_5_1-2 | `1e8b7efa…` | match |
| 640x480_5_3-4 | `c3d55529…` | match |
| 640x480_7_1-2 | `91dd0807…` | match |
| 640x480_7_3-4 | `1474a79f…` | match |

The canonical vector gives 30/30 identical frames:

```
0…9   1280x720  1382400 bytes   identical
10…29 640x360    345600 bytes   identical
```

### Remaining limitation: 8-aligned height

The firmware rounds the reported output height up to a multiple of 8 and does
not provide the VP9 render height anywhere else (the FILL_BUFFER_DONE
extradata is not an input-crop payload, and there is no per-frame property).
Vectors whose resized height is not a multiple of 8 therefore still expose the
aligned height:

| Vector | true render | firmware reports |
|---|---|---|
| 320x180_* | 160x90 | 160x96 |
| 320x240_5_3-4 / 7_3-4 | 240x180 | 240x184 |
| 640x360_* | 480x270 | 480x272 |
| 1280x720_*_3-4 | 960x540 | 960x544 |
| 1920x1080_* | 960x540 | 960x544 |

Recovering the true render height needs bitstream (VP9 uncompressed header)
parsing in the kernel or a firmware change; it is out of scope for this fix.
All vectors whose resized height is a multiple of 8 now match the software
decoder exactly.

## Reproduction

```sh
# driver
sudo nabu-iris/scripts/load-module.sh \
  nabu-main/out/drivers/media/platform/qcom/iris/qcom-iris.ko

# client (the installed custom libavcodec is only used with LD_LIBRARY_PATH)
FF=$HOME/.local/opt/ffmpeg-iris-8.0.1/bin/patched_ffmpeg
L=$HOME/.local/opt/ffmpeg-iris-8.0.1/lib
V=iris-vaapi/benchmark-results/logs/fluster/resources/VP9-TEST-VECTORS/\
vp90-2-21-resize_inter_1280x720_5_1-2.webm/\
vp90-2-21-resize_inter_1280x720_5_1-2.webm

LD_LIBRARY_PATH=$L $FF -nostdin -v error -c:v vp9_v4l2m2m -i "$V" \
  -fps_mode passthrough -vf format=yuv420p -noautoscale -f md5 -
# MD5=dac2d22fc158e751de72918b8755ac1d
```

The kernel log records `Iris1 v155: VP9 resize frame=… origin=… crop=…`
once per detected transition.
