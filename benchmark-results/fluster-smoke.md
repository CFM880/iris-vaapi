# Fluster conformance smoke test

测试日期：2026-09-08。这是五个标准向量的冒烟子集，不是完整套件通过率。
Fluster 校验输入文件 checksum，并将解码后的像素 MD5 与上游参考值比较；
没有重新生成或修改参考结果。软件解码 5/5 通过，VA-API 2/5 通过。

| 向量 | 格式 | FFmpeg 软件 | FFmpeg VA-API |
|---|---|---|---|
| AUD_MW_E | H.264 Constrained Baseline | 通过 | 30 秒超时，0 帧，end picture 报错 |
| AMP_A_Samsung_7 | HEVC Main | 通过 | CAPTURE Broken pipe，surface 同步失败，输出 3 帧后失败 |
| DBLK_A_MAIN10_VIXS_4 | HEVC Main10 | 通过 | MD5 一致 |
| vp90-2-00-quantizer-00.webm | VP9 Profile 0 | 通过 | MD5 一致 |
| vp92-2-20-10bit-yuv420.webm | VP9 Profile 2 | 通过 | REQBUFS OUTPUT Invalid argument，0 帧 |

另一次不运行 H.264 的复查仍复现 HEVC Main 与 VP9 Profile 2 错误。
没有重载内核模块，因此尚不能完全排除固件会话状态影响，也尚未定位根因。
VP9 10-bit 样本为 160×90，后续应检查硬件尺寸约束与用户态公布能力是否一致。
失败是具体向量的结果，不能推广为整个 profile 不可用。

环境：

- Fluster：`f3ad284a9e6cac70dc01b02e0de71c2994181d34`
- 项目 HEAD：`141bc1ac5419bae99f70ebe6caa38bfc9d33cae3`
- 实测现有 `build/vpu_drv_video.so` SHA256：
  `21ae44883dee9e0b7814cea3c897b0f357a5f16f54bbe07eae211166efd3c67f`
- FFmpeg：`8.0.1-3ubuntu2`；内核：`6.14.11-nabu-audio1`
- `LIBVA_DRIVER_NAME=vpu`，`LIBVA_DRIVERS_PATH=<repo>/build`
- `cached_capture=Y`；`vainfo` 确认 vpu-vaapi 0.2.0 / qcom-iris
- 单任务串行，每个向量超时 30 秒；使用上游 FFmpeg VA-API 后端，日志确认 Iris 设备。

复现（从项目根目录运行，需要现有驱动构建和可用硬件）：

```sh
git clone https://github.com/fluendo/fluster.git /tmp/iris-fluster
git -C /tmp/iris-fluster checkout f3ad284a9e6cac70dc01b02e0de71c2994181d34
python3 benchmarks/fluster_smoke.py /tmp/iris-fluster
```

若 checkout 已存在，跳过 clone。脚本仅下载选定的五个向量，保留上游参考 MD5，
运行六个软件/VA-API 后端并返回 Fluster 的退出码；本轮因解码失败返回 1。
`--build-dir` 和 `--output-dir` 可覆盖默认目录。

原始 JSON 摘要保存在 [fluster-smoke.json](fluster-smoke.json)。本机完整日志、
下载码流、子集定义和复查结果位于忽略目录 `logs/fluster-smoke/`。

后续扩大测试应使用 JVT-AVC_V1、JCT-VC-HEVC_V1、VP9-TEST-VECTORS 和
VP9-TEST-VECTORS-HIGH；将硬件不支持的 profile、4:2:2、4:4:4、12-bit 等单独
归类，不能直接用套件总通过率衡量驱动质量，也不能未经验证就跳过失败向量。

Fluster 耗时包含进程启动、解码、像素读回、格式转换和 MD5，不能作为纯硬件吞吐。
尤其本版 JSON 的超时用例 `total_time` 未包含完整等待时间，应以向量 `time` 和
日志为准。原有性能基准仍用于 fps/CPU/RSS，对照此测试衡量正确性。
