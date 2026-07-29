cmake_minimum_required(VERSION 3.16)
project(unified_capture LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

add_compile_options(-Wall -g -O2)

# SDK paths may be overridden for an RK3588 target environment.
set(MPP_INC "/usr/include/rockchip" CACHE PATH "Rockchip MPP include path")
set(SURVIVE_DIR "/root/projects/libsurvive" CACHE PATH "libsurvive source and runtime path")

add_executable(unified_capture
    app/main.cpp
    app/runtime.cpp
    app/status_response.cpp
    app/socket_server.cpp
    app/gpio_control.cpp
    app/session_runner.cpp
    app/session_profile.cpp
    core/product_config.cpp
    hardware/common/sensor.cpp
    hardware/video/device_discovery.cpp
    hardware/wrist/wrist_profile.cpp
    hardware/wrist/wrist_discovery.cpp
    hardware/as5600/as5600.c
)

target_include_directories(unified_capture PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${MPP_INC}
    ${SURVIVE_DIR}/include/libsurvive
    ${SURVIVE_DIR}/include
    ${SURVIVE_DIR}/redist
    ${SURVIVE_DIR}/libs/cnmatrix/include
    ${SURVIVE_DIR}/libs/cnkalman/src
)

target_link_directories(unified_capture PRIVATE
    ${SURVIVE_DIR}/bin
)

target_link_libraries(unified_capture PRIVATE
    rockchip_mpp
    turbojpeg
    gpiod
    survive
    pthread
    rt
    udev
    m
)

set_target_properties(unified_capture PROPERTIES
    BUILD_RPATH "${SURVIVE_DIR}/bin"
    INSTALL_RPATH "${SURVIVE_DIR}/bin"
)
