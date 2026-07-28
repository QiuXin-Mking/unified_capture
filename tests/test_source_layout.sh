#!/bin/sh
set -eu
for path in app/main.cpp app/runtime.h app/runtime.cpp app/socket_server.h app/socket_server.cpp app/gpio_control.h app/gpio_control.cpp app/session_runner.h app/session_runner.cpp core/barrier.h core/camera_config.h core/frame_queue.h core/output_path.h core/product_config.h core/product_config.cpp core/time_utils.h hardware/common/sensor.h hardware/common/sensor.cpp hardware/video/device_discovery.h hardware/video/device_discovery.cpp hardware/video/video_sensor.h hardware/video/sixcam_sensor.h hardware/video/bgr2nv12.h hardware/video/mpp_encoder.h hardware/imu/imu_sensor.h hardware/imu/imu_decode.h hardware/as5600/as5600.c hardware/as5600/as5600.h hardware/as5600/encoder_sensor.h hardware/tracker/vive_tracker_sensor.h hardware/tracker/vive_usb.h hardware/wrist/wrist_profile.h hardware/wrist/wrist_profile.cpp hardware/wrist/wrist_discovery.h hardware/wrist/wrist_discovery.cpp deploy/unified_capture.service; do test -f "$path"; done
for path in main.cpp barrier.h camera_config.h frame_queue.h output_path.h time_utils.h vive_usb.h unified_capture.service; do test ! -e "$path"; done
for path in hardware/IMU/ hardware/VideoSensor/; do
	if git ls-files | grep -Eq "^$path"; then
		echo "tracked legacy path: $path" >&2
		exit 1
	fi
done
! grep -Eq '#include "(hardware/|app/(gpio_control|session_runner|socket_server)\.h")' app/main.cpp
! grep -Eq '_exit|Nori_Xvision|gpiod_|socket\(|accept\(|poll\(|VideoSensor|SixCamSensor|ImuSensor' app/main.cpp

help_output=$(make help)
printf '%s\n' "$help_output" | grep -Fqx '  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \'
printf '%s\n' "$help_output" | grep -Fqx '       NORI_INC=/path/to/nori/include NORI_LIB=/path/to/nori/lib'
! printf '%s\n' "$help_output" | grep -F '\\n+'
