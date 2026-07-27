# Makefile — 简易构建 (替代 CMake)
#
# 用法:
#   make                # 编译
#   make clean          # 清理
#   make scan           # 扫描设备
#
# 编译前确认:
#   1. Nori Xvision SDK 已安装, Nori_Xvision_API.h 在 include 路径中
#   2. Rockchip MPP 已安装
#   3. libturbojpeg-dev, libgpiod-dev 已安装

CXX      ?= g++
CC       ?= gcc
CXXFLAGS := -std=c++20 -Wall -g -O2 -pthread
CFLAGS   := -std=gnu11 -Wall -g -O2
LDFLAGS  := -lNori_Xvision_Std -lrockchip_mpp -lturbojpeg -lgpiod -lsurvive -lpthread -lrt -ludev -lm

# Nori Xvision SDK 路径 (按需调整)
NORI_INC ?= /usr/local/Nori_Xvision/include
NORI_LIB ?= /usr/local/Nori_Xvision/lib
MPP_INC  ?= /usr/include/rockchip

# libsurvive 路径
SURVIVE_DIR ?= /root/projects/libsurvive

INCLUDES := -I. \
	-I$(NORI_INC)/Nori_Xvision_API \
	-I$(MPP_INC) \
	-I$(SURVIVE_DIR)/include/libsurvive \
	-I$(SURVIVE_DIR)/include \
	-I$(SURVIVE_DIR)/redist \
	-I$(SURVIVE_DIR)/libs/cnmatrix/include \
	-I$(SURVIVE_DIR)/libs/cnkalman/src

LIBS     := -L$(NORI_LIB) -L$(SURVIVE_DIR)/bin \
	-Wl,-rpath,$(NORI_LIB) \
		-Wl,-rpath,$(SURVIVE_DIR)/bin \
	$(LDFLAGS)

TARGET   := unified_capture
OBJS     := main.o hardware/as5600/as5600.o

.PHONY: all clean scan test_hardware_header_layout test_time_utils

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

main.o: main.cpp sensor.h camera_config.h barrier.h time_utils.h output_path.h \
        hardware/VideoSensor/VideoSensor.h hardware/VideoSensor/SixCamSensor.h \
        hardware/VideoSensor/bgr2nv12.h hardware/VideoSensor/mpp_encoder.h \
        hardware/IMU/ImuSensor.h hardware/IMU/imu_decode.h encoder_sensor.h \
        hardware/tracker/ViveTrackerSensor.h hardware/tracker/resample_grid.h \
        frame_queue.h vive_usb.h hardware/as5600/as5600.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ main.cpp

hardware/as5600/as5600.o: hardware/as5600/as5600.c hardware/as5600/as5600.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ hardware/as5600/as5600.c

scan: $(TARGET)
	./$(TARGET) --scan

clean:
	rm -f $(TARGET) $(OBJS)

test_output_path: test_output_path.cpp output_path.h
	$(CXX) $(CXXFLAGS) -o $@ test_output_path.cpp

test_hardware_header_layout:
	sh tests/test_hardware_header_layout.sh

test_time_utils: test_time_utils.cpp time_utils.h
	$(CXX) $(CXXFLAGS) -o $@ test_time_utils.cpp

help:
	@echo "Unified Capture Build"
	@echo ""
	@echo "Targets:"
	@echo "  make        Build $(TARGET)"
	@echo "  make scan   Build and scan Nori devices"
	@echo "  make clean  Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  Nori_INC   Nori SDK header path (default: /usr/local/Nori/include)"
	@echo "  Nori_LIB   Nori SDK lib path (default: /usr/local/Nori/lib)"
	@echo "  MPP_INC    Rockchip MPP header path (default: /usr/include/rockchip)"
	@echo ""
	@echo "Build on RK3588:"
	@echo "  make"
	@echo ""
	@echo "Cross-compile:"
	@echo "  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \\"
	@echo "       Nori_INC=/path/to/tstc/include Nori_LIB=/path/to/tstc/lib"
