#pragma once

#include <optional>
#include <string>
#include <string_view>

enum class ProductProfile { mango, banana };

struct WristDeviceMap {
    bool allow_missing_devices = false;
    std::string left_product;
    std::string right_product;
};

struct ProductConfiguration {
    ProductProfile profile = ProductProfile::mango;
    WristDeviceMap wrist;
};

struct ProductConfigResult {
    std::optional<ProductConfiguration> configuration;
    std::string error;
};

ProductConfigResult load_product_configuration(
    const std::string& product_config_path,
    const std::string& camera_map_path);
std::string_view product_profile_name(ProductProfile profile);
