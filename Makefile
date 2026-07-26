# Makefile — 简易构建 (替代 CMake)
#
# 用法:
#   make                # 编译
#   make clean          # 清理
#   make scan           # 扫描设备
#
# 编译前确认:
#   1. TSTC SDK 已安装, USBCam_API.h 在 include 路径中
#   2. Rockchip MPP 已安装
#   3. libturbojpeg-dev, libgpiod-dev 已安装

CXX      ?= g++
CC       ?= gcc
CXXFLAGS := -std=c++20 -Wall -g -O2 -pthread
CFLAGS   := -std=gnu11 -Wall -g -O2
LDFLAGS  := -lUSBCam_API -lrockchip_mpp -lturbojpeg -lgpiod -lsurvive -lpthread -lrt -ludev -lm

# TSTC SDK 路径 (按需调整)
TSTC_INC ?= /usr/local/TSTC/include
TSTC_LIB ?= /usr/local/TSTC/lib
MPP_INC  ?= /usr/include/rockchip

# libsurvive 路径
SURVIVE_DIR ?= /root/projects/libsurvive

INCLUDES := -I. \
	-I$(TSTC_INC)/USBCam_API \
	-I$(MPP_INC) \
	-I$(SURVIVE_DIR)/include/libsurvive \
	-I$(SURVIVE_DIR)/include \
	-I$(SURVIVE_DIR)/redist \
	-I$(SURVIVE_DIR)/libs/cnmatrix/include \
	-I$(SURVIVE_DIR)/libs/cnkalman/src

LIBS     := -L$(TSTC_LIB) -L$(SURVIVE_DIR)/bin \
	-Wl,-rpath,$(SURVIVE_DIR)/bin \
	$(LDFLAGS)

TARGET   := unified_capture
OBJS     := main.o as5600.o

.PHONY: all clean scan

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

main.o: main.cpp sensor.h camera_config.h barrier.h time_utils.h output_path.h \
        video_sensor.h sixcam_sensor.h imu_sensor.h encoder_sensor.h \
        hardware/tracker/ViveTrackerSensor.h hardware/tracker/resample_grid.h \
        mpp_encoder.h frame_queue.h imu_decode.h vive_usb.h as5600.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ main.cpp

as5600.o: as5600.c as5600.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ as5600.c

scan: $(TARGET)
	./$(TARGET) --scan

clean:
	rm -f $(TARGET) *.o

test_output_path: test_output_path.cpp output_path.h
	$(CXX) $(CXXFLAGS) -o $@ test_output_path.cpp

help:
	@echo "Unified Capture Build"
	@echo ""
	@echo "Targets:"
	@echo "  make        Build $(TARGET)"
	@echo "  make scan   Build and scan TSTC devices"
	@echo "  make clean  Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  TSTC_INC   TSTC SDK header path (default: /usr/local/TSTC/include)"
	@echo "  TSTC_LIB   TSTC SDK lib path (default: /usr/local/TSTC/lib)"
	@echo "  MPP_INC    Rockchip MPP header path (default: /usr/include/rockchip)"
	@echo ""
	@echo "Build on RK3588:"
	@echo "  make"
	@echo ""
	@echo "Cross-compile:"
	@echo "  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \\"
	@echo "       TSTC_INC=/path/to/tstc/include TSTC_LIB=/path/to/tstc/lib"
