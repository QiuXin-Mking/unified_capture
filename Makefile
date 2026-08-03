# Makefile — unified capture production build and host-only tests
#
# Production builds require Rockchip MPP, libturbojpeg, libgpiod, and libsurvive.

CXX      ?= g++
CC       ?= gcc
CXXFLAGS := -std=c++20 -Wall -g -O2 -pthread
CFLAGS   := -std=gnu11 -Wall -g -O2
LDFLAGS  := -lrockchip_mpp -lturbojpeg -lgpiod -lsurvive -lpthread -lrt -ludev -lm

MPP_INC  ?= /usr/include/rockchip

# libsurvive path (override as needed).
SURVIVE_DIR ?= /root/projects/libsurvive

INCLUDES := -I. \
	-I$(MPP_INC) \
	-I$(SURVIVE_DIR)/include/libsurvive \
	-I$(SURVIVE_DIR)/include \
	-I$(SURVIVE_DIR)/redist \
	-I$(SURVIVE_DIR)/libs/cnmatrix/include \
	-I$(SURVIVE_DIR)/libs/cnkalman/src

LIBS     := -L$(SURVIVE_DIR)/bin \
	-Wl,-rpath,$(SURVIVE_DIR)/bin \
	$(LDFLAGS)

TARGET := unified_capture
CPP_SOURCES := app/main.cpp app/runtime.cpp app/status_response.cpp app/socket_server.cpp \
	app/gpio_control.cpp app/session_runner.cpp \
	app/session_profile.cpp \
	core/product_config.cpp hardware/common/sensor.cpp \
	hardware/video/device_discovery.cpp \
	hardware/wrist/wrist_profile.cpp hardware/wrist/wrist_discovery.cpp
C_SOURCES := hardware/as5600/as5600.c
CPP_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(CPP_SOURCES))
C_OBJECTS := $(patsubst %.c,build/obj/%.o,$(C_SOURCES))
OBJS := $(CPP_OBJECTS) $(C_OBJECTS)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean scan test test_product_config test_cherry_product_config test_wrist_discovery test_session_profile test_status_response test_output_path test_time_utils test_video_capture_control test_capture_output_policy test_socket_command test_compressed_frame_queue test_video_pipeline_stats test_v4l2_frame_view test_yuv_to_nv12 test_imu_luma_decode test_imu_frame_queue test_capture_pipeline test_async_frame_sink test_cherry_protocol test_source_layout help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

build/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

-include $(DEPS)

scan: $(TARGET)
	./$(TARGET) --scan

test: test_product_config test_cherry_product_config test_wrist_discovery test_session_profile test_status_response test_output_path test_time_utils test_video_capture_control test_capture_output_policy test_socket_command test_compressed_frame_queue test_video_pipeline_stats test_v4l2_frame_view test_yuv_to_nv12 test_imu_luma_decode test_imu_frame_queue test_capture_pipeline test_async_frame_sink test_cherry_protocol test_source_layout

test_product_config: build/tests/test_product_config
	./$<

build/tests/test_product_config: tests/test_product_config.cpp core/product_config.cpp core/product_config.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_product_config.cpp core/product_config.cpp

test_cherry_product_config: build/tests/test_cherry_product_config
	./$<

build/tests/test_cherry_product_config: tests/test_cherry_product_config.cpp core/product_config.cpp core/product_config.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_cherry_product_config.cpp core/product_config.cpp

test_wrist_discovery: build/tests/test_wrist_discovery
	./$<

build/tests/test_wrist_discovery: tests/test_wrist_discovery.cpp \
	hardware/wrist/wrist_profile.cpp hardware/wrist/wrist_discovery.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^

test_session_profile: build/tests/test_session_profile
	./$<

build/tests/test_session_profile: tests/test_session_profile.cpp app/session_profile.cpp app/session_profile.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_session_profile.cpp app/session_profile.cpp

test_status_response: build/tests/test_status_response
	./$<

build/tests/test_status_response: tests/test_status_response.cpp app/status_response.cpp app/status_response.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_status_response.cpp app/status_response.cpp

test_output_path: build/tests/test_output_path
	./$<

build/tests/test_output_path: tests/test_output_path.cpp core/output_path.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_time_utils: build/tests/test_time_utils
	./$<

build/tests/test_time_utils: tests/test_time_utils.cpp core/time_utils.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_video_capture_control: build/tests/test_video_capture_control
	./$<

build/tests/test_video_capture_control: tests/test_video_capture_control.cpp hardware/video/capture_control.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_capture_output_policy: build/tests/test_capture_output_policy
	./$<

build/tests/test_capture_output_policy: tests/test_capture_output_policy.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_socket_command: build/tests/test_socket_command
	./$<

build/tests/test_socket_command: tests/test_socket_command.cpp app/socket_server.h app/socket_server.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_socket_command.cpp app/socket_server.cpp

test_compressed_frame_queue: build/tests/test_compressed_frame_queue
	./$<

build/tests/test_compressed_frame_queue: tests/test_compressed_frame_queue.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_video_pipeline_stats: build/tests/test_video_pipeline_stats
	./$<

build/tests/test_video_pipeline_stats: tests/test_video_pipeline_stats.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_v4l2_frame_view: build/tests/test_v4l2_frame_view
	./$<

build/tests/test_v4l2_frame_view: tests/test_v4l2_frame_view.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_yuv_to_nv12: build/tests/test_yuv_to_nv12
	./$<

build/tests/test_yuv_to_nv12: tests/test_yuv_to_nv12.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_imu_luma_decode: build/tests/test_imu_luma_decode
	./$<

build/tests/test_imu_luma_decode: tests/test_imu_luma_decode.cpp hardware/imu/imu_decode.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Wno-unused-function $(INCLUDES) -o $@ $<

test_imu_frame_queue: build/tests/test_imu_frame_queue
	./$<

build/tests/test_imu_frame_queue: tests/test_imu_frame_queue.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_capture_pipeline: build/tests/test_capture_pipeline
	./$<

build/tests/test_capture_pipeline: tests/test_capture_pipeline.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_async_frame_sink: build/tests/test_async_frame_sink
	./$<

build/tests/test_async_frame_sink: tests/test_async_frame_sink.cpp hardware/video/async_frame_sink.h core/bounded_queue.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

test_cherry_protocol: build/tests/test_cherry_protocol
	./$<

build/tests/test_cherry_protocol: tests/test_cherry_protocol.cpp hardware/cherry/cherry_protocol.cpp hardware/cherry/cherry_protocol.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_cherry_protocol.cpp hardware/cherry/cherry_protocol.cpp

test_source_layout:
	sh tests/test_source_layout.sh

clean:
	rm -rf build $(TARGET)

help:
	@echo "Unified Capture Build"
	@echo ""
	@echo "Targets:"
	@echo "  make                     Build $(TARGET)"
	@echo "  make scan                Build and scan V4L2 devices"
	@echo "  make test                Run host-only tests"
	@echo "  make clean               Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  MPP_INC    Rockchip MPP include path (default: /usr/include/rockchip)"
	@echo ""
	@echo "Cross-compile:"
	@echo "  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \\"
	@echo "       MPP_INC=/path/to/rockchip/include"
