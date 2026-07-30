# demos/Makefile — V4L2 Y8 Capture Demos
#
# Board-native build:  make
# Cross-compile:       make CXX=aarch64-linux-gnu-g++

CXX      ?= g++
CXXFLAGS := -std=c++20 -Wall -g -O2 -pthread

# Only MJPEG demos need turbojpeg
MJPEG_LIBS := -lturbojpeg

TARGETS := demo1_yuyv_4cam demo2_yuyv_2cam \
           demo3_mjpeg_4cam demo4_mjpeg_2cam

.PHONY: all clean

all: $(TARGETS)

demo1_yuyv_4cam: demo1_yuyv_4cam.cpp v4l2_capture.h
	$(CXX) $(CXXFLAGS) -o $@ $<

demo2_yuyv_2cam: demo2_yuyv_2cam.cpp v4l2_capture.h
	$(CXX) $(CXXFLAGS) -o $@ $<

demo3_mjpeg_4cam: demo3_mjpeg_4cam.cpp v4l2_capture.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(MJPEG_LIBS)

demo4_mjpeg_2cam: demo4_mjpeg_2cam.cpp v4l2_capture.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(MJPEG_LIBS)

clean:
	rm -f $(TARGETS)
