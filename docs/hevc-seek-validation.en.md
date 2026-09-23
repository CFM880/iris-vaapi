# HEVC seek stale-frame issue: validation guide (Intel / cross-platform)

English | [中文](hevc-seek-validation.md)

Companion bug report: [`chromium-bug-m153-hevc-seek.md`](chromium-bug-m153-hevc-seek.md).
This guide is for reproducing the issue on **other VA-API platforms such as
Intel**, and for re-testing after a Chromium fix.

---

## 1. Recap

- Symptom: with hardware HEVC decode on Chrome **153**, seeking during playback
  makes the picture periodically jump back to a frame decoded before the seek;
  the decoder keeps producing frames (`totalVideoFrames` keeps rising).
- Root cause: the M153 feature **`ExtendedVideoBitstreamValidation`** (default
  on) plus `H265Decoder::Reset()` clearing `active_sps_`, which makes every
  seek look like a configuration change.
- Independent of the driver; **HEVC only**, H.264/VP9/AV1 are unaffected.

Expected validation results:

| Config | stale repeats |
|---|---|
| Chrome 153, default (feature on) | ~70–90% |
| Chrome 153, `--disable-features=ExtendedVideoBitstreamValidation` | 0–5% |
| Chrome 152 | 0–5% |

---

## 2. Platform prerequisites (Intel 10th-gen i9)

10th-gen i9 iGPU:
- Comet Lake: UHD 630 (Gen9.5)
- Ice Lake: Iris Plus (Gen11)

Both support HEVC 8/10-bit hardware decode.

```sh
# 1) Is the iGPU present
lspci -nnk | grep -iA3 vga

# 2) VA-API driver (either one; iHD is preferred for 10th-gen)
sudo apt install intel-media-va-driver      # iHD, works for Comet Lake/Ice Lake
# or sudo apt install i965-va-driver        # legacy driver, Gen9.5 only

# 3) Verify HEVC support
vainfo --display drm --device /dev/dri/renderD128
#   Need to see:
#     VAProfileHEVCMain      : VAEntrypointVLD
#     VAProfileHEVCMain10    : VAEntrypointVLD
```

If there is no `VAProfileHEVC*`, the iGPU/driver does not support HEVC hardware
decode; use another machine or validate via the Windows D3D11 path.

---

## 3. Clip and harness

- Clip: 4K HEVC 8-bit MP4, preferably ≥30 s (the harness seeks every 4 s, 7
  times). Replace the out-of-tree path as needed.
- Harness: `iris-vaapi/benchmarks/seek_harness/`
  - `seq_server.py`: local HTTP server (Range support) + collects page reports
  - `seek_test.html`: plays, seeks on a schedule, reports a 48x27 grayscale hash
  - `analyze.py`: measures the fraction of post-seek samples repeating content
    seen before a seek (`stale_repeats`)

> On Intel there is no iris-vaapi frame-serial barcode (`VPU_FRAME_STAMP`), so
> the `sn`/`stale_serial` fields are meaningless; **use `stale_repeats` only**.

---

## 4. Reproduce / measure

```sh
cd iris-vaapi
VID=/path/to/4k-hevc-8bit.mp4

# Start the server
SEEK_VIDEO="$VID" SEEK_LOG=/tmp/seq.log SEEK_PORT=8756 \
  python3 benchmarks/seek_harness/seq_server.py &

# Chrome 153, hardware decode (usually on by default on Intel; add flags if needed)
LIBVA_DRIVER_NAME=iHD \
/opt/google/chrome/google-chrome \
  --user-data-dir=/tmp/chrome-seek --ozone-platform=wayland --start-fullscreen \
  --enable-features=VaapiVideoDecoder,VaapiVideoEncoder,PlatformHEVCDecoderSupport \
  --ignore-gpu-blocklist \
  http://127.0.0.1:8756/

# Let it play for ~36 s, close Chrome, then analyze
python3 benchmarks/seek_harness/analyze.py /tmp/seq.log
```

> **Always use the real binary.** If a wrapper on the machine injects
> `--disable-features=ExtendedVideoBitstreamValidation` (typically
> `~/.local/bin/google-chrome-stable`, which shadows the real binary because
> `~/.local/bin` precedes `/usr/bin` in `PATH`), then every "default" run has
> the feature forced off and the regression looks fixed. Reproduce and A/B with
> `/opt/google/chrome/google-chrome` directly. Measured on `153.0.8010.47` with
> the real binary, the issue **still reproduces** (stale 86.6%, one
> `ApplyResolutionChange` per seek); the relevant sources are byte-identical to
> `153.0.8010.36`.

First confirm hardware decode is active: in `chrome://media-internals` the
decoder should be `VaapiVideoDecoder`, and in `chrome://gpu` "Video Decode:
Hardware accelerated".

### A/B comparison

Run the same 153 binary again with
`--disable-features=ExtendedVideoBitstreamValidation`; optionally a third run
with Chrome 152. Record `stale_repeats` for each.

### Log evidence available on Intel (optional)

```sh
google-chrome-stable --enable-logging=stderr \
  --vmodule=vaapi_video_decoder=3 --user-data-dir=/tmp/chrome-log ... 2>/tmp/chrome.log
grep -c 'ApplyResolutionChange()' /tmp/chrome.log
```

- Feature on: about **6** `ApplyResolutionChange()` calls in a 24 s / 5-seek run
  (1 at startup + 1 per seek).
- Feature off: about **1** (startup only).

---

## 5. Results table

Measured on 2026-09-15 (Qualcomm/Intel) and 2026-09-23 (Windows):

| Platform | Session | Clip | feature on stale | feature off stale | `ApplyResolutionChange` on/off |
|---|---|---|---|---|---|
| SM8150/Adreno640 (nabu, iris-vaapi) | Wayland | 4K Jellyfin | 74–87% | **0%** | 6 / 1 |
| SM8150/Adreno640 (nabu) | Wayland | 1080p closed-GOP transcode | **88.0%** (settle=15) | **0%** | — |
| Intel CometLake-H UHD (iHD 26.3.2) | X11 | 4K Jellyfin | 6.1% | 5.0% | **8 / 1** |
| Intel CometLake-H UHD | X11 | 1080p closed-GOP transcode | 9.6% | 7.7% | **14 / 2** |
| Intel CometLake-H UHD | Wayland(GNOME) | 4K Jellyfin | 6.8–8.2% | 6.7–9.1% | — |
| Intel CometLake-H UHD | Wayland(GNOME) | 1080p closed-GOP transcode | 12.2% | 8.0% | — |
| Windows (RTX 3080, `D3D11VideoDecoder`, Chrome 154.0.8037.58) | desktop | 4K Jellyfin | 2.7% (settle=5), 3.7% (settle=15) | 10.5%, 9.7% | **8 / 1** |
| Windows (RTX 3080, `D3D11VideoDecoder`) | desktop | 1080p closed-GOP synthetic, 210 s | **0.0%** | **0.0%** | — |

Conclusions (important):
- **The defect is platform-independent**: on both platforms with the feature on,
  every seek triggers `ApplyResolutionChange` (Intel's 8/1 and 14/2 confirm it).
- **The visible stale-frame symptom only reproduces reliably on nabu** (Qualcomm
  iris-vaapi stable-surface reuse + Wayland/ANGLE). On Intel, whether X11 or
  Wayland(GNOME), there is no on/off separation (noise floor ~7–9%); raising
  `analyze.py`'s settle from 5 to 15 does not change that (an earlier one-off
  Intel 18.8% was settle contamination). In other words, the symptom also needs
  the nabu display/frame-pool path.
- **Windows/D3D11 (Chrome 154, RTX 3080) confirms the defect but not the
  symptom.** `chrome://media-internals` shows `D3DVideoDecoder config change` +
  `RecreateDecoderWrapper` once per seek: 8 with the feature on, 1 with it off
  (the D3D11 equivalent of `ApplyResolutionChange`). The visible symptom does
  not reproduce — there is no on/off separation, and a 210 s clip whose 7 seek
  targets are all distinct gives 0.0% for both. The 2.7% vs 10.5% on the 30 s
  clip is seek-target revisit noise, not stale frames.

(nabu on Chrome 152 is 0%; H.264 hardware/software are both 0%.)

### About settle (noise floor)

`analyze.py` now takes a second argument for the number of samples skipped after
each seek: `analyze.py <log> [settle]` (default 5). Seek settling is slower on
Intel and the noise floor is higher, but even at 15 there is no on/off
separation, so the Intel noise is not the symptom.

### Intel-specific notes

- On this Intel host Chrome does not get a render node by default
  (`vaapi_wrapper.cc GetHandle() ... failed to find a suitable render node`), so
  HEVC is not supported at all. Add
  `--render-node-override=/dev/dri/renderD128` to use VA-API/HEVC. `mpv
  --hwdec=vaapi` works normally.
- Transcodes must use **closed GOP + parameter sets repeated at every IRAP**:
  `ffmpeg -i 4K.mp4 -vf scale=1920:1080 -c:v libx265 -preset ultrafast \
   -x265-params keyint=60:min-keyint=60:open-gop=0:repeat-headers=1 -an out.mp4`
  A default open-GOP (CRA) transcode stalls after seek (`ct` stops advancing)
  and cannot be used to judge.

---

## 6. Other platforms

- **Windows (D3D11)**: hardware HEVC uses the same `H265Decoder`
  (`media/gpu/windows/d3d_video_decoder.cc`, `kConfigChange` →
  `RecreateDecoderWrapper()`). Verified on Chrome 154 / RTX 3080: the spurious
  config change fires once per seek (8 with the feature on, 1 with it off), but
  the visible stale-frame symptom does not reproduce (see section 5).
- **macOS (VideoToolbox)**: also based on `H265Decoder`, but whether the display
  layer shows stale frames is not verified.
- **V4L2 stateful / others**: do not go through this logic and are out of scope.

---

## 7. Caveats

- Linux Chromium has **no HEVC software fallback**;
  `--disable-accelerated-video-decode` simply fails to play, so software decode
  cannot be used as a control.
- Use an **HEVC** clip; H.264 is 0% by nature and cannot be used to judge.
- **Check the launcher for an injected flag first**: wrappers such as
  `~/.local/bin/google-chrome-stable` append
  `--disable-features=ExtendedVideoBitstreamValidation`, turning the feature off
  even in "default" runs; reproduce and A/B with the real binary
  `/opt/google/chrome/google-chrome`.
- Only look at `stale_repeats`; a "normal one- or two-frame rollback" after a
  seek is settle, and `analyze.py` already skips the first 5 samples of each
  segment.
- `/tmp` may be cleaned by the system; keep important logs/screenshots in a
  persistent directory outside the repository.

---

## 8. Checklist

- [ ] `vainfo` shows `VAProfileHEVCMain` / `Main10`
- [ ] `chrome://gpu` Video Decode = Hardware accelerated
- [ ] `chrome://media-internals` decoder = `VaapiVideoDecoder`
- [ ] Using the real binary (`/opt/google/chrome/google-chrome`), not a wrapper
      that injects `--disable-features=ExtendedVideoBitstreamValidation`
- [ ] Chrome 153 default: `stale_repeats` is high (expect 70–90%)
- [ ] Chrome 153 + `--disable-features=ExtendedVideoBitstreamValidation`: ~0%
- [ ] (optional) Chrome 152: ~0%
- [ ] (optional) `ApplyResolutionChange` count: on ≈ once per seek, off ≈ only
      at startup
