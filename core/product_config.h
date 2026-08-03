#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ProductProfile { mango, banana, cherry };

struct WristDeviceMap {
    bool allow_missing_devices = false;
    std::string left_product;
    std::string right_product;
};

struct CherryDeviceMap {
    uint16_t vid = 0x5268;
    uint16_t pid = 0x1218;
    int width = 3200;
    int height = 1200;
    int fps = 30;
    std::string format = "H264";
    bool allow_missing_devices = true;
    std::string wrist_left_product;
    std::string wrist_right_product;
};

struct ProductConfiguration {
    ProductProfile profile = ProductProfile::mango;
    WristDeviceMap wrist;
    CherryDeviceMap cherry;
    bool sixcam_enabled = false;  // banana: include six-camera module
};

struct ProductConfigResult {
    std::optional<ProductConfiguration> configuration;
    std::string error;
};

ProductConfigResult load_product_configuration(
    const std::string& product_config_path,
    const std::string& camera_map_path);
std::string_view product_profile_name(ProductProfile profile);
