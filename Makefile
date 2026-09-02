CC ?= gcc
CFLAGS ?= -O2
# VA driver callbacks intentionally leave some ABI parameters unused.
CFLAGS += -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden
CPPFLAGS += $(shell pkg-config --cflags libva 2>/dev/null)
LDLIBS += -lva -ldl -pthread

ifneq ($(shell pkg-config --exists vulkan 2>/dev/null && echo yes),)
CPPFLAGS += -DVPU_HAVE_VULKAN $(shell pkg-config --cflags vulkan)
LDLIBS += $(shell pkg-config --libs vulkan)
endif

BUILD = build
DRIVER = $(BUILD)/vpu_drv_video.so
QCOM_V4L2_OBJ = $(BUILD)/qcom_v4l2_decoder.o
PLATFORM_CORE_OBJ = $(BUILD)/platform.o
QCOM_PLATFORM_OBJ = $(BUILD)/qcom_iris.o
PLATFORM_OBJS = $(PLATFORM_CORE_OBJ) $(QCOM_PLATFORM_OBJ) $(QCOM_V4L2_OBJ)
CODEC_CORE_OBJ = $(BUILD)/codec.o
H264_CODEC_OBJ = $(BUILD)/h264_codec.o
HEVC_CODEC_OBJ = $(BUILD)/hevc_codec.o
VP9_CODEC_OBJ = $(BUILD)/vp9_codec.o
CODEC_OBJS = $(CODEC_CORE_OBJ) $(H264_CODEC_OBJ) $(HEVC_CODEC_OBJ) $(VP9_CODEC_OBJ)
H264_OBJ = $(BUILD)/h264_params.o
HEVC_OBJ = $(BUILD)/hevc_params.o
HEVC_REWRITE_OBJ = $(BUILD)/hevc_slice_rewrite.o
VK_COPY_OBJ = $(BUILD)/vk_copy.o
TEST_VA = $(BUILD)/test_va
TEST_V4L2 = $(BUILD)/test_v4l2_dec
TEST_H264 = $(BUILD)/test_h264_params
TEST_VADEC = $(BUILD)/test_va_decode
TEST_VP9 = $(BUILD)/test_va_vp9
TEST_V4L2_VP9 = $(BUILD)/test_v4l2_vp9
TEST_HEVC = $(BUILD)/test_hevc_au
TEST_HEVC_PARAMS = $(BUILD)/test_hevc_params
TEST_HEVC_REWRITE = $(BUILD)/test_hevc_slice_rewrite
TEST_VA_STRESS = $(BUILD)/test_va_stress
TEST_SURFACE_FENCE = $(BUILD)/test_surface_fence
TEST_PLATFORM = $(BUILD)/test_platform
TEST_CODEC = $(BUILD)/test_codec

.PHONY: all check clean install uninstall

DRIVERDIR ?= $(shell pkg-config --variable=driverdir libva 2>/dev/null)
DESTDIR ?=

all: $(DRIVER) $(TEST_VA) $(TEST_V4L2) $(TEST_H264) $(TEST_VADEC) \
	$(TEST_VP9) $(TEST_V4L2_VP9) $(TEST_HEVC) $(TEST_HEVC_PARAMS) $(TEST_HEVC_REWRITE) \
	$(TEST_VA_STRESS) $(TEST_SURFACE_FENCE) $(TEST_PLATFORM) $(TEST_CODEC)

DECODE_OBJ = $(BUILD)/decode.o

$(DRIVER): src/vaapi.c $(BUILD)/decode.o $(PLATFORM_OBJS) $(CODEC_OBJS) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ) $(VK_COPY_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC -shared $(CPPFLAGS) -Isrc -o $@ $< $(DECODE_OBJ) $(PLATFORM_OBJS) $(CODEC_OBJS) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ) $(VK_COPY_OBJ) $(LDFLAGS) $(LDLIBS)

$(DECODE_OBJ): src/decode.c src/decode.h src/codec/types.h src/platform/platform.h src/codec/codec.h src/vk_copy.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/decode.c

$(PLATFORM_CORE_OBJ): src/platform/platform.c src/platform/platform.h src/platform/platform_internal.h src/codec/types.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/platform/platform.c

$(QCOM_PLATFORM_OBJ): src/platform/qcom/iris.c src/platform/platform.h src/platform/platform_internal.h src/platform/qcom/v4l2_decoder.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/platform/qcom/iris.c

$(CODEC_CORE_OBJ): src/codec/codec.c src/codec/codec.h src/codec/codec_internal.h src/codec/types.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/codec/codec.c

$(H264_CODEC_OBJ): src/codec/h264/h264_codec.c src/codec/codec.h src/codec/codec_internal.h src/codec/h264/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/codec/h264/h264_codec.c

$(HEVC_CODEC_OBJ): src/codec/hevc/hevc_codec.c src/codec/codec.h src/codec/codec_internal.h src/codec/hevc/hevc_params.h src/codec/hevc/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/codec/hevc/hevc_codec.c

$(VP9_CODEC_OBJ): src/codec/vp9/vp9_codec.c src/codec/codec.h src/codec/codec_internal.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/codec/vp9/vp9_codec.c

$(VK_COPY_OBJ): src/vk_copy.c src/vk_copy.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -g -fPIC $(CPPFLAGS) -Isrc -c -o $@ src/vk_copy.c

$(HEVC_OBJ): src/codec/hevc/hevc_params.c src/codec/hevc/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(HEVC_REWRITE_OBJ): src/codec/hevc/hevc_slice_rewrite.c src/codec/hevc/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(QCOM_V4L2_OBJ): src/platform/qcom/v4l2_decoder.c src/platform/qcom/v4l2_decoder.h src/platform/qcom/surface_fence.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -c -o $@ $<

$(TEST_VA): test/test_va.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_V4L2): test/platform/qcom/test_v4l2_decoder.c $(QCOM_V4L2_OBJ) src/platform/qcom/v4l2_decoder.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(QCOM_V4L2_OBJ)

$(H264_OBJ): src/codec/h264/h264_params.c src/codec/h264/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_H264): test/codec/test_h264_params.c $(H264_OBJ) src/codec/h264/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(H264_OBJ)

$(TEST_VADEC): test/test_va_decode.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_VP9): test/test_va_vp9.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_V4L2_VP9): test/platform/qcom/test_vp9.c $(QCOM_V4L2_OBJ) src/platform/qcom/v4l2_decoder.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(QCOM_V4L2_OBJ)

$(TEST_HEVC): test/platform/qcom/test_hevc.c $(QCOM_V4L2_OBJ) src/platform/qcom/v4l2_decoder.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(QCOM_V4L2_OBJ)

$(TEST_HEVC_PARAMS): test/codec/test_hevc_params.c $(HEVC_OBJ) src/codec/hevc/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_OBJ)

$(TEST_HEVC_REWRITE): test/codec/test_hevc_slice_rewrite.c $(HEVC_REWRITE_OBJ) src/codec/hevc/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_REWRITE_OBJ)

$(TEST_VA_STRESS): test/test_va_stress.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lva-drm

$(TEST_SURFACE_FENCE): test/platform/qcom/test_surface_fence.c src/platform/qcom/surface_fence.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $<

$(TEST_PLATFORM): test/platform/test_platform.c $(PLATFORM_OBJS) src/platform/platform.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(PLATFORM_OBJS)

$(TEST_CODEC): test/codec/test_codec.c $(CODEC_OBJS) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ) src/codec/codec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(CODEC_OBJS) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ)

clean:
	rm -rf $(BUILD)

check: $(TEST_H264) $(TEST_HEVC_PARAMS) $(TEST_HEVC_REWRITE) $(TEST_PLATFORM) $(TEST_CODEC)
	./$(TEST_H264)
	./$(TEST_HEVC_PARAMS)
	./$(TEST_HEVC_REWRITE)
	./$(TEST_PLATFORM)
	./$(TEST_CODEC)

install: $(DRIVER)
	test -n "$(DRIVERDIR)"
	install -d "$(DESTDIR)$(DRIVERDIR)"
	install -m 0755 $(DRIVER) "$(DESTDIR)$(DRIVERDIR)/vpu_drv_video.so"

uninstall:
	test -n "$(DRIVERDIR)"
	rm -f "$(DESTDIR)$(DRIVERDIR)/vpu_drv_video.so"
