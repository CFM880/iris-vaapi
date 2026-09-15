# [M153 regression] HEVC hardware decode: seeking repeatedly displays stale pre-seek frames

> Ready-to-submit Chromium bug report. Primary measurement on Xiaomi Pad 5
> (SM8150 / Adreno 640, iris-vaapi, Wayland). The spurious configuration change
> was independently confirmed on a second VA-API platform, Intel Comet Lake-H
> UHD (iHD, X11). The root cause is in shared Chromium H.265 code
> (`H265Decoder`), used by the VA-API, D3D11 and VideoToolbox accelerators.
> See `hevc-seek-validation.md` for the reproduction procedure and the
> cross-platform result table.

## Summary

With hardware HEVC decode on Linux (Wayland), Chrome **153.0.8010.36** presents
stale frames from **before a seek** after seeking. The decoder keeps running
(`totalVideoFrames` keeps rising) but the compositor repeatedly shows a frame
whose content was decoded before the seek. Chrome **152.0.7977.82** on the
identical machine, kernel, driver and clip is clean.

Bisected to the M153 feature **`ExtendedVideoBitstreamValidation`** (default
on). `--disable-features=ExtendedVideoBitstreamValidation` restores correct
behavior. It is HEVC-specific: H.264 hardware/software and 4K H.264 are clean.

Root cause: in M153 `H265Decoder::Reset()` was changed to clear `active_sps_`,
while configuration-change detection now uses
`!active_sps_ || *active_sps_ != *sps`. Because a seek resets the decoder, the
first SPS after every seek is misdetected as a configuration change, producing a
spurious `kConfigChange` → `ApplyResolutionChange()` / decoder+pool recreation
on every seek, which manifests as stale frames.

Cross-platform: the spurious configuration change is **platform-independent** and
was confirmed on Intel Comet Lake-H UHD (iHD): `ApplyResolutionChange()` runs on
every seek with the feature on (8/1 for the 4K clip, 14/2 for a 1080p clip) and
only once with it off. On that Intel host the *visible* stale-frame symptom did
not reproduce on either X11 or Wayland/GNOME (stale repeats ~7–9% both ways,
unchanged by a larger post-seek settle window), so the visible symptom
additionally depends on the display / frame-pool path — it is strongly
reproducible on the Qualcomm iris-vaapi + Wayland/ANGLE setup (88% vs 0%).

## Regression range

| Build | Result |
|---|---|
| 152.0.7977.82 | OK |
| 153.0.8010.36 | BROKEN |
| 153.0.8010.36 + `--disable-features=ExtendedVideoBitstreamValidation` | OK |

## Environment

- Device: Xiaomi Pad 5 (Qualcomm SM8150, Adreno 640), aarch64
- OS: Linux kernel `6.14.11-nabu1`, Wayland
- Chrome: `153.0.8010.36` (control: `152.0.7977.82`)
- Decoder: `VaapiVideoDecoder` (`chrome://media-internals`)
- VA-API driver: downstream Qualcomm Iris driver (`iris-vaapi`), HEVC Main 8-bit
- Clip: `Test Jellyfin 4K HEVC 8bit 60M.mp4` 3840x2160@60, ~30 s, ~60 Mbps

The driver is not a variable: the same driver produces 0% stale frames on 152
and on 153 with H.264, and the result does not change across two driver builds
or across driver/kernel cache-coherency variants.

## Steps to reproduce

1. Play a 4K HEVC 8-bit MP4 with hardware decode:
   `google-chrome-stable --ozone-platform=wayland --enable-features=VaapiVideoDecoder <url>`
2. Seek several times during playback (e.g. every 4 s).
3. The picture repeatedly jumps back to pre-seek content while playback
   position and `totalVideoFrames` advance; `droppedVideoFrames` climbs.

Quantify with the `benchmarks/seek_harness/` scripts (in this repo): the page
seeks on a fixed schedule and reports, for each displayed frame, an FNV hash of
a 48x27 downsample. `analyze.py` reports the fraction of post-seek samples whose
hash was already seen before a `SEEK` marker ("stale repeats").

## Actual vs expected

- Expected: presented frames advance monotonically with playback position.
- Actual: post-seek samples repeat pre-seek content. Measured stale repeats:

| Config | Stale repeats (153) |
|---|---|
| HEVC VA-API, feature on (default) | 74–87% across runs |
| HEVC VA-API, feature off | 0% (0/333, 0/335, ...) |
| HEVC VA-API, Chrome 152 | 0% |
| H.264 VA-API (1080p and 4K) | 0% |
| H.264 software | 0% |

## Bisection / mechanism evidence

`Chrome 153` with verbose logging
(`--enable-logging=stderr --vmodule=vaapi_video_decoder=3`) shows
`VaapiVideoDecoder::ApplyResolutionChange()` on **every seek** when the feature
is on, and only once (startup) when it is off:

| Config | `ApplyResolutionChange()` calls in a 24 s / 5-seek run | Stale repeats |
|---|---|---|
| feature on (default) | 6 (startup + one per seek) | 74.5% |
| feature off | 1 (startup only) | 0% |

The clip was verified to be conformant: a single SPS repeated 150x (identical
bytes) and all 1797 VCL NALs have `first_slice_segment_in_pic_flag == 1`. So the
new "non-first slice without prior slice" and "non-IRAP SPS change" rejections
are not reacting to malformed input; the config change is a state bug.

## Root cause in code

`media/gpu/h265_decoder.cc` (M153):

```c
void H265Decoder::Reset() {
  ...
  parser_.Reset();
  accelerator_->Reset();

  active_sps_.reset();          // <-- added by 2d15788560
  decoder_buffer_.reset();
  secure_handle_ = 0;
  state_ = kAfterReset;
}

bool H265Decoder::ProcessPPS(...) {
  ...
  bool is_config_change = false;
  if (parser_.validate_extended_bitstream()) {
    is_config_change = !active_sps_ || *active_sps_ != *sps;   // <-- 2d15788560
  } else {
    is_config_change = pic_size_ != new_pic_size ||
                       dpb_.max_num_pics() != sps->max_dpb_size || ...;
  }
  ...
}
```

`Reset()` clears `active_sps_` but leaves the other SPS-derived state
(`pic_size_`, `profile_`, `bit_depth_`, `chroma_sampling_`, DPB size) intact, so
clearing only `active_sps_` is inconsistent and makes the first post-seek SPS
look like a configuration change.

## Suspect CLs

- `2d15788560` "media: Reject all non-IRAP H.265 SPS configuration changes"
  (bug 540024134) — introduced `active_sps_` as the comparison source and
  `active_sps_.reset()` in `Reset()`; **primary suspect**.
- `5dc7ba4f4e` "media: Reject HEVC non-first slice segment when prior slice is
  missing" (bug 536470854) — gated by the same feature, but not triggered by a
  conformant stream (no non-first slices), so not the cause here.

Feature introduction: `a67403c92953761dd1cdda758967210581763b7d`
("media: Add range extended validation for H.264 and H265 parsers"), whose
comment says the feature is meant to be temporary ("Remove after M149").

## Proposed fix

Minimal, upstream:

```diff
--- a/media/gpu/h265_decoder.cc
+++ b/media/gpu/h265_decoder.cc
@@ -202,7 +202,6 @@ void H265Decoder::Reset() {
   parser_.Reset();
   accelerator_.Reset();

-  active_sps_.reset();
   decoder_buffer_.reset();
   secure_handle_ = 0;
```

`active_sps_` is only used to detect an actual SPS change. Keeping it across a
seek makes an unchanged SPS a no-op; a genuinely different SPS is still caught
by `*active_sps_ != *sps`. The first-ever parse still has `!active_sps_` true,
so initial configuration is unaffected.

Alternative (if the reset must stay): trigger `!active_sps_` only on the first
parse of the decoder's lifetime, via a flag that is not cleared by `Reset()`.

## Scope

`H265Decoder` is shared by:
- Linux/ChromeOS VA-API (`media/gpu/vaapi/vaapi_video_decoder.cc`),
- Windows D3D11 (`media/gpu/windows/d3d11_video_decoder.cc:195`),
- VideoToolbox accelerator implementations.

So the spurious config-change defect is platform-independent and affects
hardware HEVC on all these backends. It was confirmed on two VA-API setups:
Qualcomm SM8150 (iris-vaapi) and Intel Comet Lake-H (iHD). The visible
stale-frame symptom is demonstrated on the Qualcomm/Wayland setup; it did not
reproduce on Intel (X11 and Wayland/GNOME) in our tests, and whether Windows/macOS
show stale frames is not verified. H.264/VP9/AV1 use different decoders and are
unaffected.

## Workaround

- `--disable-features=ExtendedVideoBitstreamValidation`, or
- stay on Chrome 152.

## Attachments / references

- `benchmarks/seek_harness/` (seq_server.py, seek_test.html, analyze.py)
- Verify procedure and expected results: `docs/hevc-seek-validation.md`
