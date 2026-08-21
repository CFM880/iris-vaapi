CC ?= gcc
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra
CPPFLAGS += $(shell pkg-config --cflags libva 2>/dev/null)
LDFLAGS += -lva -ldl

BUILD = build
DRIVER = $(BUILD)/iris_drv_video.so
V4L2_OBJ = $(BUILD)/v4l2_dec.o
TEST_VA = $(BUILD)/test_va
TEST_V4L2 = $(BUILD)/test_v4l2_dec

.PHONY: all clean install

all: $(DRIVER) $(TEST_VA) $(TEST_V4L2)

$(DRIVER): src/iris_vaapi.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -fPIC -shared $(CPPFLAGS) -o $@ $< $(LDFLAGS)

$(V4L2_OBJ): src/v4l2_dec.c src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(TEST_VA): test/test_va.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS)

$(TEST_V4L2): test/test_v4l2_dec.c $(V4L2_OBJ) src/v4l2_dec.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc -o $@ $< $(V4L2_OBJ)

clean:
	rm -rf $(BUILD)