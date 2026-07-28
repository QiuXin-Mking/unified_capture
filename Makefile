# Makefile — unified capture production build and host-only tests
#
# Production builds require the Nori SDK, Rockchip MPP, and libsurvive.

CXX      ?= g++
CC       ?= gcc
CXXFLAGS := -std=c++20 -Wall -g -O2 -pthread
CFLAGS   := -std=gnu11 -Wall -g -O2
LDFLAGS  := -lNori_Xvision_Std -lrockchip_mpp -lturbojpeg -lgpiod -lsurvive -lpthread -lrt -ludev -lm

# Nori Xvision SDK paths (override as needed).
NORI_INC ?= /usr/local/Nori_Xvision/include
NORI_LIB ?= /usr/local/Nori_Xvision/lib
MPP_INC  ?= /usr/include/rockchip

# libsurvive path (override as needed).
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

TARGET := unified_capture
CPP_SOURCES := app/main.cpp app/runtime.cpp app/socket_server.cpp \
	app/gpio_control.cpp app/session_runner.cpp \
	hardware/common/sensor.cpp hardware/video/device_discovery.cpp
C_SOURCES := hardware/as5600/as5600.c
CPP_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(CPP_SOURCES))
C_OBJECTS := $(patsubst %.c,build/obj/%.o,$(C_SOURCES))
OBJS := $(CPP_OBJECTS) $(C_OBJECTS)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean scan test test_output_path test_time_utils test_video_capture_control test_socket_command test_source_layout help

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

test: test_output_path test_time_utils test_video_capture_control test_socket_command test_source_layout

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

test_socket_command: build/tests/test_socket_command
	./$<

build/tests/test_socket_command: tests/test_socket_command.cpp app/socket_server.h app/socket_server.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tests/test_socket_command.cpp app/socket_server.cpp

test_source_layout:
	sh tests/test_source_layout.sh

clean:
	rm -rf build $(TARGET)

help:
	@echo "Unified Capture Build"
	@echo ""
	@echo "Targets:"
	@echo "  make                     Build $(TARGET)"
	@echo "  make scan                Build and scan Nori devices"
	@echo "  make test                Run host-only tests"
	@echo "  make clean               Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  NORI_INC   Nori SDK include path (default: /usr/local/Nori_Xvision/include)"
	@echo "  NORI_LIB   Nori SDK library path (default: /usr/local/Nori_Xvision/lib)"
	@echo "  MPP_INC    Rockchip MPP include path (default: /usr/include/rockchip)"
	@echo ""
	@echo "Cross-compile:"
	@echo "  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \\\n+	       NORI_INC=/path/to/nori/include NORI_LIB=/path/to/nori/lib"
