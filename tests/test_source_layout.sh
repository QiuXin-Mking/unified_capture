#!/bin/sh
set -eu
for path in app/main.cpp app/runtime.h app/runtime.cpp app/socket_server.h app/socket_server.cpp app/gpio_control.h app/gpio_control.cpp app/session_runner.h app/session_runner.cpp app/session_profile.h app/session_profile.cpp core/barrier.h core/camera_config.h core/frame_queue.h core/output_path.h core/product_config.h core/product_config.cpp core/time_utils.h hardware/common/sensor.h hardware/common/sensor.cpp hardware/video/device_discovery.h hardware/video/device_discovery.cpp hardware/video/v4l2_device.h hardware/video/video_sensor.h hardware/video/sixcam_sensor.h hardware/video/bgr2nv12.h hardware/video/mpp_encoder.h hardware/imu/imu_sensor.h hardware/imu/imu_decode.h hardware/as5600/as5600.c hardware/as5600/as5600.h hardware/as5600/encoder_sensor.h hardware/tracker/vive_tracker_sensor.h hardware/tracker/vive_usb.h hardware/wrist/wrist_profile.h hardware/wrist/wrist_profile.cpp hardware/wrist/wrist_discovery.h hardware/wrist/wrist_discovery.cpp deploy/unified_capture.service; do test -f "$path"; done
test -f tests/test_mango_wrist_socket.sh
bash -n tests/test_mango_wrist_socket.sh

# Cherry production, test, configuration, deployment, and analysis artifacts.
for path in \
	hardware/cherry/cherry_discovery.h hardware/cherry/cherry_discovery.cpp \
	hardware/cherry/cherry_protocol.h hardware/cherry/cherry_protocol.cpp \
	hardware/cherry/cherry_h264_writer.h hardware/cherry/cherry_h264_writer.cpp \
	hardware/cherry/cherry_process_utils.h hardware/cherry/cherry_start_control.h \
	hardware/cherry/cherry_serial_sensor.h hardware/cherry/cherry_serial_sensor.cpp \
	hardware/cherry/cherry_video_sensor.h hardware/cherry/cherry_video_sensor.cpp \
	tests/test_cherry_product_config.cpp tests/test_cherry_discovery.cpp \
	tests/test_cherry_protocol.cpp tests/test_cherry_h264_writer.cpp \
	tests/test_cherry_start_control.cpp tests/test_cherry_json.cpp \
	tests/test_cherry_serial_lifecycle.cpp tests/test_cherry_process_utils.cpp \
	tests/test_calc_cherry_sync.py tests/test_sync_to_rk3588.sh \
	deploy/product.conf.example deploy/camera-map.conf.example \
	deploy/calc_cherry_sync.py deploy/sync_to_rk3588.sh \
	docs/design/cherry-profile-requirements.md; do
	test -f "$path"
done
bash -n deploy/sync_to_rk3588.sh
bash -n deploy/test_yuyv_concurrency.sh
bash -n tests/test_sync_to_rk3588.sh

for source in cherry_discovery.cpp cherry_protocol.cpp cherry_h264_writer.cpp cherry_serial_sensor.cpp cherry_video_sensor.cpp; do
	grep -Fq "hardware/cherry/$source" Makefile
done

aggregate=$(grep '^test:' Makefile)
for target in test_calc_cherry_sync test_sync_to_rk3588 test_cherry_product_config \
	test_cherry_discovery test_cherry_protocol test_cherry_h264_writer \
	test_cherry_start_control test_cherry_json test_cherry_serial_lifecycle \
	test_cherry_process_utils; do
	case " $aggregate " in
		*" $target "*) ;;
		*) echo "make test is missing $target" >&2; exit 1 ;;
	esac
done
for path in main.cpp barrier.h camera_config.h frame_queue.h output_path.h time_utils.h vive_usb.h unified_capture.service; do test ! -e "$path"; done
for path in hardware/IMU/ hardware/VideoSensor/; do
	if git ls-files | grep -Eq "^$path"; then
		echo "tracked legacy path: $path" >&2
		exit 1
	fi
done
! grep -Eq '#include "(hardware/|app/(gpio_control|session_runner|socket_server)\.h")' app/main.cpp
! grep -Eq '_exit|Nori_Xvision|gpiod_|socket\(|accept\(|poll\(|VideoSensor|SixCamSensor|ImuSensor' app/main.cpp

# No Nori SDK references in production source
for f in app/*.cpp app/*.h core/*.h core/*.cpp hardware/video/*.h hardware/video/*.cpp hardware/wrist/*.h hardware/wrist/*.cpp; do
	test -f "$f" || continue
	! grep -q 'Nori_Xvision_API.h' "$f"
done
! grep -q 'Nori_Xvision_UnInit' app/runtime.cpp
! grep -q 'Nori_Xvision_Std' Makefile

help_output=$(make help)
printf '%s\n' "$help_output" | grep -Fqx '  make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \'
! printf '%s\n' "$help_output" | grep -F '\\n+'
