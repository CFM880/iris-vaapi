CC ?= gcc
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra
CPPFLAGS += $(shell pkg-config --cflags libva 2>/dev/null)
LDFLAGS += -lva -ldl

BUILD = build
DRIVER = $(BUILD)/iris_drv_video.so
V4L2_OBJ = $(BUILD)/v4l2_dec.o
H264_OBJ = $(BUILD)/h264_params.o
HEVC_OBJ = $(BUILD)/hevc_params.o
HEVC_REWRITE_OBJ = $(BUILD)/hevc_slice_rewrite.o
TEST_VA = $(BUILD)/test_va
TEST_V4L2 = $(BUILD)/test_v4l2_dec
TEST_H264 = $(BUILD)/test_h264_params
TEST_VADEC = $(BUILD)/test_va_decode
TEST_VP9 = $(BUILD)/test_va_vp9
TEST_HEVC = $(BUILD)/test_hevc_au
TEST_HEVC_PARAMS = $(BUILD)/test_hevc_params
TEST_HEVC_REWRITE = $(BUILD)/test_hevc_slice_rewrite

.PHONY: all clean install

all: $(DRIVER) $(TEST_VA) $(TEST_V4L2) $(TEST_H264) $(TEST_VADEC) \
	$(TEST_VP9) $(TEST_HEVC) $(TEST_HEVC_PARAMS) $(TEST_HEVC_REWRITE)

DECODE_OBJ = $(BUILD)/decode.o

$(DRIVER): src/iris_vaapi.c $(BUILD)/decode.o $(BUILD)/v4l2_dec.o $(BUILD)/h264_params.o $(BUILD)/hevc_params.o $(HEVC_REWRITE_OBJ)
	@mkdir -p $(BUILD)
	$(CC) -O0 -g -Wall -Wextra -fPIC -shared $(CPPFLAGS) -Isrc -o $@ $< $(DECODE_OBJ) $(V4L2_OBJ) $(H264_OBJ) $(HEVC_OBJ) $(HEVC_REWRITE_OBJ) $(LDFLAGS)

$(DECODE_OBJ): src/decode.c src/decode.h src/v4l2_dec.h src/h264_params.h src/hevc_params.h src/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) -O0 -g $(CFLAGS) $(CPPFLAGS) -Isrc -c -o $@ src/decode.c

$(HEVC_OBJ): src/hevc_params.c src/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(HEVC_REWRITE_OBJ): src/hevc_slice_rewrite.c src/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(V4L2_OBJ): src/v4l2_dec.c src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_VA): test/test_va.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) -lva-drm

$(TEST_V4L2): test/test_v4l2_dec.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

$(H264_OBJ): src/h264_params.c src/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_H264): test/test_h264_params.c $(H264_OBJ) src/h264_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(H264_OBJ)

$(TEST_VADEC): test/test_va_decode.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) -lva-drm

$(TEST_VP9): test/test_va_vp9.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) -lva-drm

$(TEST_HEVC): test/test_hevc_au.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

$(TEST_HEVC_PARAMS): test/test_hevc_params.c $(HEVC_OBJ) src/hevc_params.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_OBJ)

$(TEST_HEVC_REWRITE): test/test_hevc_slice_rewrite.c $(HEVC_REWRITE_OBJ) src/hevc_slice_rewrite.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(HEVC_REWRITE_OBJ)

clean:
	rm -rf $(BUILD)
