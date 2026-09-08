# Fluster 三路径初始基准

2026-09-08，通过 `make check-fluster FLUSTER_ARGS=/tmp/iris-fluster` 实测。
这是五个向量 × 三条路径的冒烟基准；完整四套测试集尚未运行。

| 向量 | 软件 | V4L2 M2M | VA-API |
|---|---|---|---|
| H.264 / AUD_MW_E | Success | Timeout | Timeout |
| HEVC Main / AMP_A_Samsung_7 | Success | Success | Error |
| HEVC Main10 / DBLK_A_MAIN10_VIXS_4 | Success | Fail（MD5） | Success |
| VP9 P0 / vp90-2-00-quantizer-00.webm | Success | Success | Success |
| VP9 P2 / vp92-2-20-10bit-yuv420.webm | Success | Error | Error |
| 合计 | 5/5 | 2/5 | 2/5 |

Success 表示与上游参考 MD5 一致。Fail 表示校验不一致，Error 表示解码命令报错，
Timeout 为 30 秒超时。脚本返回 1，Make 返回 2，正确暴露基准未通过。
另以 `--paths software --no-download` 验证缓存模式，5/5 通过且返回 0。

HEVC Main 的直接 V4L2 成功而 VA-API 失败，值得重点调查适配路径。Main10 的
V4L2 校验失败与已有 FFmpeg 10-bit CAPTURE 协商限制相容，但不能仅凭本轮结果
确认根因。测试未在每个用例之间重载固件，超时后的状态影响仍需进一步排除。
不将这些失败自动跳过，也不以 2/5 推断整个 profile 的支持程度。

Make 本轮按依赖重新构建了驱动，二进制哈希以元数据为准。此前
[双路径试接入报告](fluster-smoke.md) 使用的是当时已有构建，作为历史记录保留。

- [原始 Fluster JSON](fluster-baseline.json)
- [版本、环境、二进制哈希与命令](fluster-baseline-metadata.json)
- [运行和分析指南](../docs/fluster.md)

完整日志位于元数据命令所指向的本机运行目录下 `run.log`。耗时含输出转换和
MD5，本轮以正确性和接入验证为目的，不作为 fps/CPU 性能结论。
