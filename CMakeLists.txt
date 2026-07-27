cmake_minimum_required(VERSION 3.16)
project(unified_capture LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ---- 编译选项 ----
add_compile_options(-Wall -g -O2)

# ---- TSTC SDK 路径 (根据实际安装位置调整) ----
set(TSTC_SDK_DIR "/usr/local/TSTC" CACHE PATH "TSTC USB Camera SDK path")
set(MPP_DIR "/usr" CACHE PATH "Rockchip MPP path")

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${TSTC_SDK_DIR}/include
    ${MPP_DIR}/include/rockchip
)

link_directories(
    ${TSTC_SDK_DIR}/lib
    ${MPP_DIR}/lib
)

# ---- 源文件 ----
add_executable(unified_capture
    main.cpp
    hardware/as5600/as5600.c
)

# ---- 链接库 ----
target_link_libraries(unified_capture
    # TSTC SDK
    USBCam_API

    # MPP
    rockchip_mpp

    # turbojpeg (MJPEG 解码)
    turbojpeg

    # gpiod (GPIO 按钮)
    gpiod

    # 系统库
    pthread
    rt

    # ALSA (可选, 如果不需要音频可移除)
    asound
)

# ---- 复制 ffmpeg 依赖提示 ----
# 需要在系统上安装: ffmpeg
# sudo apt install ffmpeg
