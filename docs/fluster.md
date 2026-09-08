# Fluster 测试基准

本项目采用 [Fluster](https://github.com/fluendo/fluster) 作为标准码流符合性和
回归测试框架，固定版本为 `f3ad284a9e6cac70dc01b02e0de71c2994181d34`。
脚本保留上游输入 checksum 和输出 MD5，不通过重新生成参考值消除失败。

## 快速运行

需要 Python 3、Git、带 VA-API 和 V4L2 M2M 解码器的 FFmpeg，以及本项目支持的
Iris 内核、固件和设备访问权限。首次运行需要网络，自动下载固定版 Fluster
与五个测试向量；后续复用下载缓存。

```sh
make check-fluster
```

默认比较以下三条路径，每次仅运行一个解码进程，每个向量超时 30 秒：

| 路径 | Fluster 后端 | 测试对象 |
|---|---|---|
| 软件 | FFmpeg-H.264 / FFmpeg-H.265 / FFmpeg-VP9 | 同一环境下的软件对照 |
| V4L2 | 上述名称加 -v4l2m2m | FFmpeg → V4L2 stateful → Iris |
| VA-API | 上述名称加 -VAAPI | FFmpeg → vpu-vaapi → V4L2 stateful → Iris |

默认冒烟覆盖 H.264 Baseline、HEVC Main/Main10、VP9 Profile 0/2，各一个向量。
总共 15 次解码；这是接入和快速回归检查，不代表完整符合性认证。

`make check` 仍是无需硬件的参数/平台测试。Fluster 不自动重载内核模块。
测试时应关闭其他使用 VPU 的应用，避免资源竞争影响结果。

## 完整套件和复查

```sh
# 下载并运行四套完整上游测试集，可能需要较多时间和下载空间。
make check-fluster-full

# 使用已存在、版本匹配且 tracked 文件无修改的 Fluster checkout。
make check-fluster FLUSTER_ARGS=/tmp/iris-fluster

# 额外七个原始向量，含 1080p、tiles/WPP、超长尺寸和动态分辨率。
make check-fluster FLUSTER_ARGS='--suite extended'

# 仅复查某条路径；使用下载缓存，每个向量最多等待 60 秒。
make check-fluster FLUSTER_ARGS='--paths v4l2 --no-download --timeout 60'

# 自定义输出目录，也可直接运行 Python 脚本。
python3 benchmarks/run_fluster.py --suite smoke --output-dir /tmp/iris-conformance
```

完整套件为 JVT-AVC_V1、JCT-VC-HEVC_V1、VP9-TEST-VECTORS 和
VP9-TEST-VECTORS-HIGH。不自动过滤向量或将失败改为预期通过；其中可能包含硬件
不支持的格式。分析报告时应区分能力范围外、前端限制、解码错误、MD5 不一致、
超时和测试环境问题。特别是 4:2:2、4:4:4、12-bit，以及交错等编码工具，不能
仅凭 codec 名称就假定支持。

当前 FFmpeg 的 HEVC/VP9 V4L2 10-bit CAPTURE 格式协商已有已知限制，可能输出
0 帧。此类结果说明整条 FFmpeg V4L2 路径未通过，不能直接证明硬件不支持 10-bit。
V4L2 通过而 VA-API 失败时可重点调查适配层；二者都失败仍需进一步定位，不能
仅凭结果判定内核或固件有错。

本机已验证的 P010 前端需要同时选择 FFmpeg 可执行文件和对应动态库：

```sh
PATH="$HOME/.local/opt/ffmpeg-iris-8.0.1/bin:$PATH" \
LD_LIBRARY_PATH="$HOME/.local/opt/ffmpeg-iris-8.0.1/lib" \
make check-fluster FLUSTER_ARGS='/tmp/iris-fluster --no-download'
```

2026-09-08 修复后软件为 5/5，V4L2、VA-API 各为 4/5；剩余 VP9 160×90
由固件报告不支持。同内容的 160×96 和 256×128 在 8/10-bit 下均通过软件
MD5 对照，支持最小高度限制的判断。原向量仍保留且计为失败。
详见 [修复后结果](../benchmark-results/fluster-driver-vaapi.md)。

## 结果与退出码

默认 checkout 位于 `benchmark-results/logs/fluster-checkout/`；下载资源位于
`benchmark-results/logs/fluster/resources/`。每次运行新建 UTC 时间命名的目录，
避免覆盖之前的结果，包含：

- `suites/`：本次实际使用的套件定义及参考 MD5。
- `download.log`：下载和输入 checksum 检查日志。
- `run.log`：完整 FFmpeg 命令、解码日志、失败详情。
- `results.json`：Fluster 原始结果，含各向量状态和耗时。
- `metadata.json`：版本、项目工作区状态、驱动二进制 SHA256、FFmpeg 版本、
  内核、相关环境变量、执行命令及完成后的退出码。

这些大型/本机产物默认被 Git 忽略。正式基准可将对应 JSON、元数据和人工分析
复制到 `benchmark-results/`，结果文件名按用途命名，时间保留在元数据内。

只有所有请求的后端、向量实际出现且全部 Success 才返回 0；缺少后端、Skipped、
MD5 不一致、超时、下载或解码错误均返回非零。Make 会将脚本失败报告为失败。
现阶段已有失败的基准不应当作发布通过门槛；它记录现状，便于后续修复与回归对比。

## 性能解释

Fluster 的向量时间适合观察相同环境和版本下的端到端耗时回归，但包含进程启动、
像素读回、格式转换与 MD5。短向量尤其容易受启动开销影响。固定版 Fluster 的
JSON 套件 `total_time` 对超时等待的统计不完整，应查看向量时间和原始日志。

吞吐 fps、CPU 和峰值 RSS 继续使用 `compare_decode_paths.py` 与
`compare_10bit_paths.py`。应优先确认样本通过正确性校验，再解释性能结果。
Fluster 三路径耗时不能直接归因于 VA-API 单层开销。
