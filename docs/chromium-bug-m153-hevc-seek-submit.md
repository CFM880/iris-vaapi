Summary: Chrome 153 hardware HEVC decode repeatedly presents stale pre-seek
frames after seeking. Playback position and totalVideoFrames keep advancing but
the picture jumps back to content decoded before the seek. Bisected to the M153
feature ExtendedVideoBitstreamValidation (default on); Chrome 152 is clean.

What steps will reproduce the problem?
(1) On a Qualcomm SM8150 device (Xiaomi Pad 5 / "nabu", Linux, Wayland) with
    hardware HEVC decode, start Chrome with the VA-API decoder:
    google-chrome-stable --ozone-platform=wayland \
      --enable-features=VaapiVideoDecoder <url>
    Confirm in chrome://media-internals that the decoder is VaapiVideoDecoder
    and in chrome://gpu that Video Decode is hardware accelerated.
(2) Play a 4K HEVC 8-bit MP4 (~30 s, ~60 Mbps).
(3) Seek several times during playback (e.g. every 4 s).

What is the expected result?
After each seek the displayed frame advances monotonically with the playback
position; no frame decoded before the seek is shown again.

What is the actual result?
After each seek the compositor repeatedly shows frames from before the seek
while currentTime and totalVideoFrames keep advancing; droppedVideoFrames
climbs. Quantified with a local harness (downscaled-frame hashes): 74-87% of
post-seek samples repeat pre-seek content with the feature on, 0% on Chrome 152
and 0% on 153 with --disable-features=ExtendedVideoBitstreamValidation.

Please also provide additional information:
- chrome://gpu: "Download Report to File" report to be attached.
- No crash: page and GPU process do not crash, so there are no crash ids.
- Screen recording / screenshots of the fallback can be provided on request.
- Code samples / links:
  * Patch (fix + 2 regression tests) attached: h265-reset-active-sps.patch
  * Suspect CL: 2d15788560 "media: Reject all non-IRAP H.265 SPS configuration
    changes" (bug 540024134)
  * Reproducer harness: iris-vaapi benchmarks/seek_harness/
- Safari / Firefox: not applicable. Firefox on Linux does not use this HEVC
  hardware-decode path; Safari is macOS-only. H.264 and VP9 are clean in the
  same Chrome build, so the issue is HEVC-specific.

--- Additional details (root cause, evidence, proposed fix) ---

Root cause
H265Decoder::Reset() clears active_sps_, the baseline used by the full-SPS
comparison that detects H.265 SPS configuration changes:

    bool is_config_change = !active_sps_ || *active_sps_ != *sps;

Reset() is called on every seek and, per AcceleratedVideoDecoder::Reset(), must
not flush decoder state (playback may resume from a different location).
Clearing the baseline makes the first SPS re-sent after a seek look like a
configuration change even when it is byte-identical, so H265Decoder returns
kConfigChange and the client (VaapiVideoDecoder -> PrepareChangeResolution() /
ApplyResolutionChange()) flushes and reconfigures the decoder and its output
frame pool on every seek. On display paths that keep already-imported output
surfaces (Qualcomm + iris-vaapi + Wayland/ANGLE), that surfaces stale pre-seek
frames.

Evidence
- VaapiVideoDecoder::ApplyResolutionChange() runs once per seek with the
  feature on (~6 times in a 24 s / 5-seek run) and only once at startup with it
  off.
- The clip is conformant: a single SPS repeated 150x with identical bytes; all
  VCL NALs have first_slice_segment_in_pic_flag == 1. The non-IRAP SPS-change
  rejection is therefore not reacting to malformed input; this is a state bug.
- Local HEVC-enabled build of the same M153 source, real binary, VA-API:
  unpatched -> 83.7% stale repeats and ApplyResolutionChange 8; patched
  (remove the reset) -> 0.0% and 1.

Regression range
| Build | Result |
|---|---|
| 152.0.7977.82 | OK |
| 153.0.8010.36 | BROKEN |
| 153.0.8010.36 + --disable-features=ExtendedVideoBitstreamValidation | OK |
| 153.0.8010.47 | still BROKEN (media sources identical to .36; an earlier
  "fixed" report came from a local launcher that injected the disable flag) |

Proposed fix
Remove active_sps_.reset() from H265Decoder::Reset() (1 line). The parameter-set
baseline then survives a seek, so an identical SPS is a no-op while a genuinely
different SPS is still caught by *active_sps_ != *sps. The full-SPS comparison
and the non-IRAP rejection are untouched, so the hardening tracked by 540024134
is not weakened. This matches other decoders: FFmpeg's hevc_decode_flush()
keeps the SPS/PPS across a flush, and compare_sps() reuses the existing SPS when
the content is unchanged.

The attached patch also adds two tests in media/gpu/h265_decoder_unittest.cc:
- ResetThenSameSpsDoesNotCauseConfigChange (after Reset(), re-sending the
  identical SPS/PPS must not produce kConfigChange);
- ResetThenDifferentSpsOnIrapStillConfigChanges (a different SPS at an IRAP
  after Reset() is still a config change).
H265DecoderTest.* passes 18/18, including the security test
ConfigChangeOnNonIRAP (non-IRAP SPS changes are still rejected, including when
injected after Reset()).
