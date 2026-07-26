# hardware/tracker/ Makefile — 独立编译 ViveTrackerSensor + 测试
SURVIVE_DIR ?= /root/projects/libsurvive
CXXFLAGS := -std=c++20 -O2 -g -fPIC
INCLUDES := -I$(SURVIVE_DIR)/include \
            -I$(SURVIVE_DIR)/include/libsurvive \
            -I$(SURVIVE_DIR)/redist \
            -I$(SURVIVE_DIR)/libs/cnmatrix/include \
            -I$(SURVIVE_DIR)/libs/cnkalman/src
LDFLAGS  := -L$(SURVIVE_DIR)/bin \
            -Wl,-rpath,$(SURVIVE_DIR)/bin \
            -lsurvive -lpthread -lrt -ldl -lm -lz

all: test_resample

test_resample: test_resample.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)

# 系统测试: ./test_resample [duration_sec] [target_hz] [output_dir]
test: test_resample
	fuser -k /dev/hidraw* 2>/dev/null; sleep 1
	./test_resample 15 100 /tmp/vive_test

clean:
	rm -f test_resample

.PHONY: all test clean
