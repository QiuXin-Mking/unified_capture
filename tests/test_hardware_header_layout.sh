#!/bin/sh
set -eu

test -f hardware/IMU/imu_decode.h
test -f hardware/IMU/ImuSensor.h
test -f hardware/VideoSensor/VideoSensor.h
test -f hardware/VideoSensor/SixCamSensor.h
test -f hardware/VideoSensor/bgr2nv12.h
test -f hardware/VideoSensor/mpp_encoder.h

test ! -e imu_decode.h
test ! -e imu_sensor.h
test ! -e video_sensor.h
test ! -e sixcam_sensor.h
test ! -e bgr2nv12.h
test ! -e mpp_encoder.h

grep -q '#include "hardware/IMU/ImuSensor.h"' main.cpp
grep -q '#include "hardware/VideoSensor/VideoSensor.h"' main.cpp
grep -q '#include "hardware/VideoSensor/SixCamSensor.h"' main.cpp
