# Fluster 完整套件结果（Iris 自动恢复后）

2026-09-21 在 nabu 真机运行 Fluster 完整四套（pinned `f3ad284a9e6cac70dc01b02e0de71c2994181d34`），
使用发行版 `/usr/bin/ffmpeg`（8.0.1-3ubuntu2）。与
[`fluster-full.md`](fluster-full.md) 的关键区别：**pass 之间没有重载 `qcom_iris`**，
用来验证新的自动核心恢复是否消除了「一次 fatal 错误后需要重载模块」的楔死。

- 日志：`logs/fluster/20260921T141750.399105Z/`
- 内核：`6.14.11-nabu1`
- `qcom_iris` SHA256：`19900872e45a9513445baa40c6856b0df87ca93ec088a6eab25cc098c72f6dd0`
- VA-API 驱动 SHA256：`93eac88bcd96bcf005a8cdbf7c22d6d00b07b645577c28ea6422662f6e2baf32`
- `project_revision`：`cfa84c2659c6ded0e7aa8daeb68fac3d8058224b`
- 恢复代码：`nabu-iris` 的 `iris_common.c` / `iris_state.c` / `iris_core.c` /
  `iris_probe.c` / `iris_vidc.c`（未提交）

## 1. 结果

`passed/total`，每个 pass 都跑到了**完整分母**。

| 套件（向量数） | 软件 | V4L2 M2M | VA-API |
|---|---|---|---|
| H.264 JVT-AVC_V1 (135) | 130/135 | 111/135（14 Fail/9 Error/1 Timeout） | 97/135（17 Fail/21 Error） |
| HEVC JCT-VC-HEVC_V1 (147) | 141/147 | 128/147（11 Fail/8 Error） | 130/147（7 Fail/10 Error） |
| VP9 VP9-TEST-VECTORS (305) | 242/305 | 176/305（63 Fail/66 Error） | 174/305（56 Fail/75 Error） |
| VP9 VP9-TEST-VECTORS-HIGH (6) | 6/6 | 0/6（5 Fail/1 Error） | 2/6（4 Error） |

与 [`fluster-full.md`](fluster-full.md)（2026-09-14，同样 stock FFmpeg、但当时的
恢复逻辑不完整）对比：

| 套件 | V4L2 旧 | V4L2 新 | VA-API 旧 | VA-API 新 |
|---|---|---|---|---|
| H.264 | 116/135 | 111/135 | 97/135 | 97/135 |
| HEVC | 130/147 | 128/147 | 130/147 | 130/147 |
| VP9 P0 | 176/305\*（中途中止） | **176/305（跑完）** | 174/305 | 174/305 |
| VP9 High | 0/6 | 0/6 | 2/6 | 2/6 |

\* 旧结果中 VP9 V4L2 在 8×8 向量处异常中止，是下界；本次跑完全部 305 个。

- VA-API 三条硬解路径的数字与旧「每次重载」基线**完全一致**。
- V4L2 路径相对旧基线小降（H.264 116→111、HEVC 130→128），多出的失败集中在
  `Error`；是否为 power-cycle 时序影响还是运行波动，后续再查。

## 2. 自动恢复证据

全程 `run.log` 中没有出现 `modprobe`/reload。内核日志出现 3 次自动恢复：

```text
[1062.861700] session 0x3641cf3 entered ERROR from state 2 substate 0xe6 ...
[1064.942980] Iris1 v155: power-cycling core after fatal session error
[1064.979321] Iris1 firmware booted; ...
[1065.135434] session 0x3647cf3 entered ERROR from state 2 substate 0x80 ...
[1065.153910] Iris1 v155: power-cycling core after fatal session error
[1065.187753] Iris1 firmware booted; ...
[1065.355475] session 0x6e803cf3 entered ERROR from state 2 substate 0x80 ...
[1065.376374] Iris1 v155: power-cycling core after fatal session error
[1065.410280] Iris1 firmware booted; ...
[1067.618463] VPU entered power collapse
```

每次 `entered ERROR` 后约 20–40 ms 完成 power-cycle 并重新 boot 固件，测试继续。
全程没有 `timed out waiting for HFI system response` 或 core init 失败。

## 3. 结论与说明

- 楔死已消除：完整的 H.264/HEVC/VP9 序列在**不重载**的情况下跑完，且不再出现
  旧记录中「首个失败向量后整批 `REQBUFS`/EIO」的跨 pass 污染。
- 会话内部一旦发生内部解码错误（例如 `Failed to end picture decode issue: 23`，
  来自 VAAPI `vaEndPicture`），同一码流后续包会持续 EIO；这是 per-vector 行为，
  进程在向量结束关闭 fd 后即恢复，不再影响后续向量。
- 失败分布仍以固件/前端不支持为主：tiny(<64)、4:2:2/4:4:4、10/12-bit 组合、
  场/交错、高度 >8192，以及 VP9 变尺寸；这些不是楔死问题。

## 4. 复现

```sh
cd ~/sources/nabu-linux/iris-vaapi
python3 benchmarks/run_fluster.py --suite full --no-download
# 注意：不要在 pass 之间 modprobe -r qcom_iris
```
