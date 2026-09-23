# HEVC seek check — Windows 11 / Chrome 154 D3D11

- Run: 2026-09-23
- OS: Windows 11 Pro, version 10.0.26200 (build 26200)
- Browser: Chrome 154.0.8037.58 (official build, 64-bit)
- GPU: NVIDIA GeForce RTX 3080 Laptop GPU
- Decoder: `D3D11VideoDecoder` (`kIsPlatformVideoDecoder = true`, `hevc main`,
  3840x2160) — hardware HEVC on the D3D11 path
- Clip: `Test Jellyfin 4K HEVC 8bit 60M.mp4` (3840x2160, HEVC Main 8-bit,
  60 fps, approximately 59 Mb/s, 29.95 s)
- Harness: `benchmarks/seek_harness/` (`seq_server.py` + `seek_test.html` +
  `analyze.py`); the page seeks every 4 s, 7 times per run

This checks the M153 `ExtendedVideoBitstreamValidation` HEVC seek regression
(see `../docs/hevc-seek-validation.md` and
`../docs/chromium-bug-m153-hevc-seek.md`) on the Windows D3D11 backend. The
feature string is still present in the 154 `chrome.dll`, so the
`--disable-features` A/B is meaningful.

## Per-seek configuration change: reproduces

`chrome://media-internals`, same clip, 30 s / 7 seeks:

| Config | `D3DVideoDecoder config change` count |
|---|---|
| default (feature on) | **8** = 1 startup + 1 per seek |
| `--disable-features=ExtendedVideoBitstreamValidation` | **1** = startup only |

Each config change is immediately followed by a full decoder recreation:

```
D3DVideoDecoder config change: profile: hevc main, chroma_sampling_format: 4:2:0,
    coded_size: 3840x2160, visible_rect: 0,0 3840x2160, bit_depth: 8, ...
D3DVideoDecoder is using hevc main / 4:2:0
D3DVideoDecoder producing NV12
D3DVideoDecoder: Selected NV12
D3DVideoDecoder is binding textures
D3DVideoDecoder is using D3D11 backend
D3DVideoDecoder is using single textures
```

This is the D3D11 equivalent of `VaapiVideoDecoder::ApplyResolutionChange()`:
`media/gpu/windows/d3d_video_decoder.cc` (`kConfigChange` →
`RecreateDecoderWrapper()`; the change is only skipped when the config is
unchanged *and* no picture buffers exist, which is not the case on a mid-stream
seek). The trigger is the shared `media/gpu/h265_decoder.cc` `Reset()` clearing
`active_sps_`, so the first SPS after a seek is misdetected as a configuration
change — the same 8/1 on/off signature reported for VA-API.

## Visible stale-frame symptom: does not reproduce

`analyze.py` on the same clip (~44 s runs):

| Config | stale_repeats (settle=5) | (settle=15) | droppedVideoFrames max |
|---|---|---|---|
| default (feature on) | 4/215 = 2.7% | 4/215 = 3.7% | 272 |
| `--disable-features=ExtendedVideoBitstreamValidation` | 46/521 = 10.5% | 36/521 = 9.7% | 593 |

There is no on/off separation (feature-off is not lower), so the visible
regression is not present on Windows 11 / D3D11 in this setup. With a 210 s clip
whose 7 seek targets are all distinct, both configs give **0.0%**; the 3–10%
above is seek-target revisit noise on the 30 s clip, not stale frames.

## Conclusion

On Windows 11 / D3D11 hardware HEVC the spurious per-seek configuration change
(decoder + frame-pool recreation) is confirmed and gated by
`ExtendedVideoBitstreamValidation` exactly as on VA-API (8 vs 1). The visible
stale-frame symptom does not reproduce, consistent with the earlier Intel
result: the config-change defect is platform-independent, while the visible
symptom additionally needs the display/frame-pool path (Qualcomm iris-vaapi +
Wayland/ANGLE). H.264/VP9/AV1 are unaffected.

## Reproduce

```sh
# Serve the clip (Range support) and collect the page reports.
SEEK_VIDEO="/path/to/Test Jellyfin 4K HEVC 8bit 60M.mp4" \
SEEK_LOG=/tmp/seq.log SEEK_PORT=8756 \
  python3 benchmarks/seek_harness/seq_server.py &

# Chrome 154, hardware HEVC (D3D11 on Windows), play ~44 s, then analyze.
python3 benchmarks/seek_harness/analyze.py /tmp/seq.log 5
python3 benchmarks/seek_harness/analyze.py /tmp/seq.log 15
```

The configuration-change counts come from `chrome://media-internals`; run the
default build and the one with
`--disable-features=ExtendedVideoBitstreamValidation` and compare.
