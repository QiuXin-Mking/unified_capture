#include "core/product_config.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("unified_capture_product_config_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()));
    std::filesystem::create_directories(directory);

    const auto product_config = directory / "product.conf";
    const auto camera_map = directory / "camera-map.conf";

    {
        std::ofstream output(product_config);
        output << "# product.conf\n"
               << "product=banana\n";
    }
    {
        std::ofstream output(camera_map);
        output << "# camera-map.conf\n"
               << "[banana]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n"
               << "wrist_right.product=JHHSW\n"
               << "head.enabled=false\n\n"
               << "[mango]\n"
               << "profile=legacy_head\n";
    }

    const ProductConfigResult parsed =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(parsed.configuration.has_value());
    assert(parsed.configuration->profile == ProductProfile::banana);
    assert(parsed.configuration->wrist.allow_missing_devices);
    assert(parsed.configuration->wrist.left_product == "SL");
    assert(parsed.configuration->wrist.right_product == "JHHSW");

    {
        std::ofstream output(product_config);
        output << "product=pear\n";
    }
    const ProductConfigResult unknown_product =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(!unknown_product.configuration.has_value());
    assert(unknown_product.error.find("unknown product") != std::string::npos);

    {
        std::ofstream output(product_config);
        output << "product=banana\n";
    }
    {
        std::ofstream output(camera_map);
        output << "[banana]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n";
    }
    const ProductConfigResult missing_right =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(!missing_right.configuration.has_value());
    assert(missing_right.error.find("wrist_right.product") != std::string::npos);

    std::filesystem::remove_all(directory);
    return 0;
}
