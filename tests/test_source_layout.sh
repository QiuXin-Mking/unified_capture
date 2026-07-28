#!/bin/sh
set -eu
for path in app/main.cpp core/barrier.h core/camera_config.h core/frame_queue.h core/output_path.h core/time_utils.h hardware/common/sensor.h hardware/common/sensor.cpp hardware/video/video_sensor.h hardware/video/sixcam_sensor.h hardware/video/bgr2nv12.h hardware/video/mpp_encoder.h hardware/imu/imu_sensor.h hardware/imu/imu_decode.h hardware/as5600/as5600.c hardware/as5600/as5600.h hardware/as5600/encoder_sensor.h hardware/tracker/vive_tracker_sensor.h hardware/tracker/vive_usb.h deploy/unified_capture.service; do test -f "$path"; done
for path in main.cpp barrier.h camera_config.h frame_queue.h output_path.h time_utils.h vive_usb.h unified_capture.service; do test ! -e "$path"; done
for path in hardware/IMU hardware/VideoSensor; do ! git ls-files | grep -Fqx "$path"; done
