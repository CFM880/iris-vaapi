# VP9 context continuation regression — 2026-09-08

VA-API now decodes all 30 frames of the original
vp90-2-21-resize_inter_1280x720_5_1-2.webm vector, including the inter-frame
1280x720 → 640x360 transition. Three consecutive runs match the unchanged
upstream MD5 `dac2d22fc158e751de72918b8755ac1d`.

The diagnostic command includes `-noautoscale`, required to preserve native
frame dimensions when FFmpeg writes rawvideo/MD5. The pinned Fluster decoder's
original command still enables automatic scaling; its earlier failed result
is retained, not silently reclassified. The full command and binary hashes
are in vp9-context-continuation.json.

## Fix

FFmpeg destroys/recreates VAContext on this size change, but the first new
picture references old surfaces. Closing the V4L2 session lost those firmware
references, causing a sequence-discovery timeout and one missing frame.

The driver retains a bounded set of retired VP9 engines while their surfaces
exist. A new, unopened, non-direct VP9 context can adopt one only when all
three selected reference surfaces explicitly identify the same retired owner,
the pixel format matches, and the new dimensions fit the original allocation.
Active contexts are never shared. Key/intra pictures start fresh. Retired
engines are released as their last owned surface is destroyed, or at display
termination. This does not implement growth beyond the original allocation,
direct CAPTURE continuation, or arbitrary cross-context reference imports.

Smaller surfaces receive a row-wise copy from the original CAPTURE stride,
with the source and destination UV offsets calculated separately. Export and
image mapping describe the copied surface's own layout. Bounds checks reject
short source buffers and oversized target geometry.

## Validation

- `make -j4 check all`: passed, including ownership rejection, layout, UV and
  destination-boundary tests in test/test_vp9_continuation.c.
- Original five-vector smoke: software 5/5, V4L2 4/5, VA-API 4/5, as before.
  The original 160x90 VP9 vector remains firmware-rejected.
- Native-size VP9 resize command: three consecutive 30-frame MD5 matches.
- Five 4K VA-API clips: all 120 output frames each match software frame MD5s
  for H.264, HEVC Main/Main10, VP9 Profile 0/2.

The loaded kernel has the experimental sufficient-sequence property reverted,
and guards global recovery so only explicit HFI_EVENT_SYS_ERROR resets all
sessions. Normal decoding recovered after the user reloaded that build.
The unknown-session branch itself was not deliberately fault-injected.

Direct V4L2 still exposes the original visible dimensions for the resized
VP9 frames. Its metadata/notification fix remains outstanding. The separate
HEVC MP4 edit-list/negative-timestamp issue is also still outstanding.
No permanent kernel installation or system VA-API installation was performed.

Raw logs: logs/extended-4k/vp9-context-final-{0,1,2}.log;
smoke: logs/fluster/20260908T092252.023937Z/.
