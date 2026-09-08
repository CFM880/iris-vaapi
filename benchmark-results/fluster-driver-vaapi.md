# Fluster driver and VA-API regression — 2026-09-08

Pinned Fluster: `f3ad284a9e6cac70dc01b02e0de71c2994181d34`.
Kernel: `6.14.11-nabu-audio1`, temporarily loaded final Iris candidate (9).
FFmpeg: local Iris 8.0.1 build with matching shared libraries, as recorded in metadata.
The user confirmed loading the final kernel build before this regression. Its
SHA256 is recorded in the companion metadata; the installed module is unchanged.

| Vector | Software | V4L2 | VA-API |
|---|---|---|---|
| H.264 AUD_MW_E | Pass | Pass | Pass |
| HEVC AMP_A_Samsung_7 | Pass | Pass | Pass |
| HEVC Main10 DBLK_A_MAIN10_VIXS_4 | Pass | Pass | Pass |
| VP9 quantizer-00 | Pass | Pass | Pass |
| VP9 Profile 2 160×90 | Pass | Fail | Fail |

All passes match unmodified upstream MD5s. The run correctly exits nonzero.
This is a five-vector smoke regression, not full conformance or a performance result.

The kernel fixes initial sequence/DPB accounting and avoids manufacturing LAST
when an early CAPTURE allocation already fits the first sequence. It also updates
10-bit CAPTURE negotiation. Stock FFmpeg still has a separate NV12/P010 negotiation
limitation; this result uses the locally modified FFmpeg and does not certify the
stock frontend.

The VA-API fix waits for H.264/HEVC decode-order output during surface sync.
Previously synchronous clients triggered EOS after a submission; reopening the
session then discarded reference pictures needed by subsequent inter frames.
The unchanged Fluster single-thread commands exercise this regression.
VP9 synchronous clients also prematurely ended reference lifetime. Sync now
submits the existing internal show-existing-frame release AU, then waits for
the real surface, keeping the firmware session alive. The release AU has its
own timestamp and no VA target mapping.

The remaining VP9 vector is rejected by firmware as UNSUPPORTED_STREAM at input
completion. Re-encoding the same content to both 8-bit and 10-bit VP9 gives:
160×90 rejected; 160×96 and 256×128 decoded correctly, matching software MD5s.
This supports a minimum visible-height limitation, not lack of VP9 10-bit support.
The original vector remains in the baseline and remains failed. Resized probes
are diagnostic experiments and are not counted as Fluster passes.

Validation: `make -j4 check all`; all 15 smoke combinations; four resized VP9
software/V4L2 MD5 comparisons; `git diff --check` in both repositories.
Raw logs: `logs/fluster/20260908T084012.932989Z/` and `logs/driver-debug/`.

## VP9 dimension probes

Inputs were re-encoded from the original VP9 Profile 2 Fluster vector using
system FFmpeg, `-vf scale=W:H,format=yuv420p10le -c:v libvpx-vp9
-deadline realtime -cpu-used 8` (8-bit probes use `format=yuv420p`).
These transformations diagnose dimensions; they do not replace the original
conformance input or its reference hash.

| Derived input | Software / V4L2 MD5 | VA-API after sync fix |
|---|---|---|
| 8-bit 160x96 | 945bcac6a8bc2b1c45bede9f8af95bfa | not tested |
| 8-bit 256x128 | 6f2272925461506e999975cbe088172e | not tested |
| 10-bit 160x96 | df03849b6cec39cc0ca74122964f3186 | match |
| 10-bit 256x128 | 119a407580a9cdbfdf3680f9ed90e7bf | match |

VA-API probes used system FFmpeg, `-hwaccel vaapi -threads 1`, and
`LIBVA_DRIVER_NAME=vpu LIBVA_DRIVERS_PATH=<workspace>/build`, with the same
output pixel format and MD5 muxer. Before the VP9 sync fix these commands
exited zero with incorrect MD5s; after the fix both match software.

Final loaded-module regression also rechecked both 10-bit dimension probes on
both V4L2 and VA-API; all four MD5 comparisons passed. Logs and commands are in
`logs/driver-debug/vp9-final-dimensions.json`.
