# vpu-vaapi 分层架构

## 目标与边界

驱动以“完整 access unit 输入、异步 decoded frame 输出”的 stateful VPU 为核心
契约。VA-API 对象、codec 参数重建、decode/surface 调度和平台设备操作分别演进：
新增 codec 或平台实现不应修改 `vaapi.c`，平台实现的私有头文件也不能进入
codec 与 scheduler。

这里的头文件都是驱动内部模块契约：构建和安装只产出 VA driver shared object，
不会安装 `codec/*.h` 或 `platform/*.h`。内部符号使用 hidden visibility，动态
符号表只保留 libva 初始化入口，因此不形成面向应用的 C ABI。

```text
libva client
    │
    ▼
VA frontend                 src/vaapi.c
    │  VA objects / buffers / images
    ▼
codec registry + adapters   src/codec/codec.c
    │                       src/codec/{h264,hevc,vp9}/
    │  access unit + random-access/POC metadata
    ▼
decode + surface scheduler  src/decode.c
    │  codec id, pixel format, timestamp, decoded frame
    ▼
platform contract           src/platform/platform.[ch]
    │
    ├── Qualcomm Iris       src/platform/qcom/iris.c
    │                       src/platform/qcom/v4l2_decoder.c
    └── future platform     src/platform/<vendor>/
```

codec adapter 拥有 VA codec 参数、slice ranges、参数集缓存、关键帧判断和 access
unit backing。VA frontend 通过统一的 `vpu_codec_render()` 提交 buffer；scheduler
只通过 `vpu_codec_build_access_unit()` 取得不可变 AU 视图及 POC/引用统计。VP9
release AU 等 codec-valid 内部数据也由 adapter 生成，不在 scheduler 拼 bitstream。

公共 platform 层不包含 `linux/videodev2.h`，不暴露设备私有结构，也不固定设备
节点。codec、scheduler 与 platform 共享的 codec id、像素格式和 decoded-frame
结构放在 `src/codec/types.h`。当前 Qualcomm 实现负责以下映射：

- `enum vpu_codec_id` 到 V4L2 compressed OUTPUT FOURCC；
- platform session 到一个 stateful V4L2 M2M session；
- V4L2 CAPTURE buffer 到通用 `struct vpu_decoded_frame`；
- Iris 私有 surface fence、CAPTURE 队列状态和协商后 layout；
- Iris HEVC timestamp 和 VPU5 VP9 release 行为对应的显式 quirk。

## 生命周期

每个 VA display 持有一个 `vpu_platform`，它在初始化时完成一次能力探测并缓存
codec/像素格式矩阵。每个 VA context 持有一个 `vpu_platform_session`。Context reset、seek
或 EOS 后可以关闭并重开同一个 session 对象，surface registry 仍属于 display，
因此已导出的稳定 surface 可以跨 context 生命周期存在。

platform 通过以下环境变量选择：

- `VPU_PLATFORM`：平台名称；默认/`auto` 按注册顺序选择第一个实际公布
  解码能力的平台（当前只有 `qcom-iris`）；
- `VPU_DEVICE`：平台自己解释的设备路径，Qualcomm Iris 默认
  `/dev/video0`。

## 增加新平台

1. 在 `src/platform/<vendor>/` 新增实现，适配
   `platform_internal.h` 的 `struct vpu_platform_ops`。平台 SDK/内核头文件只能
   出现在该平台目录或更低层。
2. 将 ops 注册到 `platform.c` 的 `platforms[]`，并在 `Makefile` 加入对象。
3. 准确实现 `supports()`；VA-API 公布的 profile 会由 codec × NV12/P010 能力
   自动推导，不要在 VA 前端添加平台判断。
4. 正常完成的 frame 应尽量回传 submit timestamp。无法做到时，用最小范围的
   platform quirk 描述差异；不要在 core 中按设备名判断。
5. 对不支持的可选操作返回 `-ENOTSUP`。至少需要实现 session open/close、
   submit/start/poll、input/frame dequeue/requeue、event、flush 和 capture layout。
6. 扩展 `test/platform/test_platform.c`，再运行 `make check`。硬件平台还需运行
   `test_va`、对应 codec decode test，并验证 context teardown、EOS 和 seek。

当前契约服务于 stateful access-unit decoder。若平台只提供 stateless/request API，
应新增更高层的提交模型版本，而不是把 request/control 细节塞进现有 ops。

## 增加 codec

1. 在 `src/codec/<codec>/` 实现 `codec_internal.h` 的
   `struct vpu_codec_ops`。adapter 自己持有所有 VA 参数结构、slice 数据、序列
   参数缓存和 AU 内存；params/parser/rewrite helper 也留在同一私有目录。
2. 在 `codec.c` 注册 ops 和公开 profile。VA profile 枚举、codec id 与 NV12/P010
   映射会自动提供给 VA frontend 和 platform 能力过滤。
3. `render()` 必须检查每种 VA buffer 的完整尺寸与 element count；未知、与解码
   无关的 buffer type 可以忽略。
4. `build_access_unit()` 返回 adapter 所有、在下一次 render/begin 前有效的内存，
   并报告 random-access、POC 以及可选统计。不要直接操作 platform session 或
   surface。
5. 将 adapter 对象加入 `CODEC_OBJS`，扩展 `test/codec/test_codec.c`，并用真实
   硬件分别验证普通播放、EOS、seek 和参数集缓存重建。
