#include "core/product_config.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const ProductConfigResult shipped_default = load_product_configuration(
        "deploy/product.conf.example", "deploy/camera-map.conf.example");
    assert(shipped_default.configuration.has_value());
    assert(shipped_default.configuration->profile == ProductProfile::mango);

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
               << "product=mango\n";
    }
    {
        std::ofstream output(camera_map);
        output << "# camera-map.conf\n"
               << "[mango]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n"
               << "wrist_right.product=JHHSW\n"
               << "sixcam.enabled=true\n";
    }

    const ProductConfigResult parsed =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(parsed.configuration.has_value());
    assert(parsed.configuration->profile == ProductProfile::mango);
    assert(parsed.configuration->wrist.allow_missing_devices);
    assert(parsed.configuration->wrist.left_product == "SL");
    assert(parsed.configuration->wrist.right_product == "JHHSW");
    assert(!parsed.configuration->sixcam_enabled);  // mango 双目档忽略 sixcam.enabled
    assert(product_profile_name(ProductProfile::cherry) == "cherry");

    // parse_product_profile maps valid names and rejects unknowns.
    assert(parse_product_profile("mango") == ProductProfile::mango);
    assert(parse_product_profile("banana") == ProductProfile::banana);
    assert(parse_product_profile("cherry") == ProductProfile::cherry);
    assert(!parse_product_profile("pear").has_value());
    assert(!parse_product_profile("").has_value());

    // load_product_configuration_for_profile builds config without a product.conf.
    const ProductConfigResult mango_by_profile =
        load_product_configuration_for_profile(ProductProfile::mango,
                                               camera_map.string());
    assert(mango_by_profile.configuration.has_value());
    assert(mango_by_profile.configuration->profile == ProductProfile::mango);
    assert(mango_by_profile.configuration->wrist.left_product == "SL");

    // write_product_profile persists atomically and reads back as the new profile.
    assert(write_product_profile(product_config.string(), ProductProfile::banana));
    {
        std::ifstream input(product_config);
        std::string line;
        std::getline(input, line);
        assert(line == "product=banana");
    }
    const ProductConfigResult banana_reloaded =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(banana_reloaded.configuration.has_value());
    assert(banana_reloaded.configuration->profile == ProductProfile::banana);

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
        output << "product=mango\n";
    }
    {
        std::ofstream output(camera_map);
        output << "[mango]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n";
    }
    const ProductConfigResult missing_right =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(!missing_right.configuration.has_value());
    assert(missing_right.error.find("wrist_right.product") != std::string::npos);

    // mango_pro：parse / name / 六目配置加载
    assert(parse_product_profile("mango_pro") == ProductProfile::mango_pro);
    assert(product_profile_name(ProductProfile::mango_pro) == "mango_pro");
    {
        std::ofstream output(product_config);
        output << "product=mango_pro\n";
    }
    {
        std::ofstream output(camera_map);
        output << "[mango_pro]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n"
               << "wrist_right.product=JHHSW\n"
               << "sixcam.enabled=true\n";
    }
    const ProductConfigResult mango_pro =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(mango_pro.configuration.has_value());
    assert(mango_pro.configuration->profile == ProductProfile::mango_pro);
    assert(mango_pro.configuration->sixcam_enabled);
    assert(mango_pro.configuration->wrist.left_product == "SL");
    assert(write_product_profile(product_config.string(), ProductProfile::mango_pro));

    // mango 双目档：不写 sixcam.enabled，不应把 sixcam_enabled 置真
    {
        std::ofstream output(product_config);
        output << "product=mango\n";
    }
    {
        std::ofstream output(camera_map);
        output << "[mango]\n"
               << "allow_missing_devices=true\n"
               << "wrist_left.product=SL\n"
               << "wrist_right.product=JHHSW\n";
    }
    const ProductConfigResult mango_dual =
        load_product_configuration(product_config.string(), camera_map.string());
    assert(mango_dual.configuration.has_value());
    assert(mango_dual.configuration->profile == ProductProfile::mango);
    assert(!mango_dual.configuration->sixcam_enabled);

    std::filesystem::remove_all(directory);
    return 0;
}
