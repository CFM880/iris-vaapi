# Display chain and HDR verification — 2026-09-22

Scope: record what the nabu (Xiaomi Pad 5) Linux display chain actually supports,
so the HDR gap in `KNOWN-ISSUES.md` section 8 has evidence instead of a blank.

## Hardware

- Panel: CSOT `xiaomi,nabu-csot-nt36523` (Novatek NT36523 bridge), 1600x2560,
  120 Hz preferred mode, 148x236 mm.
- Link: dual DSI, 3 data lanes each, **CPHY**, video mode, master DSI0 / slave DSI1.
- The Xiaomi Pad 5 panel is advertised as HDR10 / Dolby Vision capable.

## Kernel display stack

- Driver: mainline `msm` (SM8150 DPU + DSI), `sm8150-xiaomi-nabu.dts`.
- Panel descriptor (`drivers/gpu/drm/panel/panel-novatek-nt36523.c`,
  `nabu_csot_desc`):
  - `bpc = 8`
  - `format = MIPI_DSI_FMT_RGB888`
  - `lanes = 3`, `is_dual_dsi = true`
  - no DSC configuration
- `connector->display_info.bpc = 8`; the DSI host programs `VID_DST_FORMAT_RGB888`.
- DPU colour features exposed by mainline: only `DPU_DSPP_PCC` (panel colour
  correction) in the SM8150 catalog; no 3D LUT / gamut / dither / memory colour.

### KMS properties (`modetest -M msm`, raw log in
`benchmark-results/logs/display-hdr/modetest.txt`)

| Object | Properties present |
|---|---|
| Connector `DSI-1` | EDID (empty, internal panel), DPMS, link-status, non-desktop, TILE |
| CRTC | VRR_ENABLED, CTM (3x3 colour-transform blob) |
| Plane | type, IN_FORMATS (incl. `AR30`/`XR30` 10-bit RGB and `P010` 10-bit YUV) |

There is **no** `HDR_OUTPUT_METADATA`, **no** `max bpc`, and **no** `Colorspace`
property on the connector, so userspace cannot signal HDR (PQ/HLG, mastering
display, MaxCLL/FALL) or request a 10-bit output. The planes can accept 10-bit
formats, but the DSI link is fixed at 8-bit RGB888.

## Compositor and playback behaviour

`mpv 0.41.0` / libplacebo 7.360.0 against the running GNOME Wayland session,
playing a true HDR10 sample
(`The World in HDR.mkv`: VP9 Profile 2, 3840x2160, `yuv420p10le`, bt.2020 /
smpte2084 (PQ) / bt.2020, with Mastering display metadata):

```
Registered interface wp_color_manager_v1 at version 2
transfer: gamma2.2, primaries: bt.709
target: min_luma=0.2, max_luma=80.0, max_cll=0.0, max_fall=0.0
VO: [gpu-next] 3840x2160 yuv420p10
```

The compositor advertises the display target as **SDR** (gamma 2.2, bt.709,
~80 nits, no HDR metadata). mpv therefore tone-maps the HDR10 stream to SDR in
libplacebo and submits an 8-bit result. Full log:
`benchmark-results/logs/display-hdr/mpv-hdr10-tone-mapping.log`.

## Conclusion

- The display chain and decoders work: 10-bit HDR10 input decodes and is shown.
- The panel is HDR-capable hardware, but **the mainline msm/DPU/DSI stack has no
  HDR path**: no HDR metadata property, no selectable bit depth, an 8-bit RGB888
  DSI link without DSC, and only a PCC/CTM colour block.
- HDR10/DV content is therefore **tone-mapped to 8-bit SDR** by userspace
  (mpv/libplacebo, or Chrome's compositor path); there is no HDR passthrough and
  no DV support.

## What an HDR path would require

1. Kernel DRM: attach `HDR_OUTPUT_METADATA` and `max bpc`/`Colorspace` to the
   msm DSI connector and forward the metadata to the DPU/DSI.
2. DPU: program the DSPP colour pipeline (gamut/3D LUT/dither) and 10-bit
   output; the mainline DPU catalog exposes only PCC today.
3. DSI/panel: switch the link to 10-bit and enable DSC to fit 1600x2560@120 over
   the CPHY lanes, plus a 10-bit panel init sequence (the current descriptor is
   8bpc RGB888).
4. Compositor: tone mapping / HDR composition (mutter needs the KMS HDR
   properties from step 1).

Until then the current behaviour (userspace tone mapping to SDR) is the correct
fallback and is what this verification records.

## Raw evidence

- `benchmark-results/logs/display-hdr/modetest.txt`
- `benchmark-results/logs/display-hdr/mpv-hdr10-tone-mapping.log`
- `benchmark-results/logs/display-hdr/hdr-sample-probe.txt`
