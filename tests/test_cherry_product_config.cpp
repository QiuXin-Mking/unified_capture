#include "core/product_config.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr const char* kValidCherryMap =
    "[cherry]\n"
    "stereo.vid=0x5268\n"
    "stereo.pid=0x1218\n"
    "stereo.resolution=3200x1200\n"
    "stereo.format=H264\n"
    "stereo.fps=30\n"
    "allow_missing_devices=true\n";

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path);
    assert(output);
    output << contents;
    assert(output);
}

ProductConfigResult load_cherry(const std::filesystem::path& product_config,
                                const std::filesystem::path& camera_map,
                                const std::string& map_contents) {
    write_file(product_config, "product=cherry\n");
    write_file(camera_map, map_contents);
    return load_product_configuration(product_config.string(), camera_map.string());
}

void assert_rejected(const std::filesystem::path& product_config,
                     const std::filesystem::path& camera_map,
                     const std::string& map_contents) {
    const ProductConfigResult result =
        load_cherry(product_config, camera_map, map_contents);
    assert(!result.configuration.has_value());
    assert(!result.error.empty());
}

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("unified_capture_cherry_product_config_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()));
    std::filesystem::create_directories(directory);
    const auto product_config = directory / "product.conf";
    const auto camera_map = directory / "camera-map.conf";

    const ProductConfigResult valid =
        load_cherry(product_config, camera_map, kValidCherryMap);
    assert(valid.configuration.has_value());
    assert(valid.configuration->profile == ProductProfile::cherry);
    assert(valid.configuration->cherry.vid == 0x5268);
    assert(valid.configuration->cherry.pid == 0x1218);
    assert(valid.configuration->cherry.width == 3200);
    assert(valid.configuration->cherry.height == 1200);
    assert(valid.configuration->cherry.fps == 30);
    assert(valid.configuration->cherry.format == "H264");
    assert(valid.configuration->cherry.allow_missing_devices);
    assert(valid.configuration->cherry.wrist_left_product.empty());
    assert(valid.configuration->cherry.wrist_right_product.empty());

    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268suffix\n"
                    "stereo.pid=0x1218\n"
                    "stereo.resolution=3200x1200\n"
                    "stereo.format=H264\n"
                    "stereo.fps=30\n"
                    "allow_missing_devices=true\n");
    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268\n"
                    "stereo.pid=0x1218\n"
                    "stereo.resolution=3200-1200\n"
                    "stereo.format=H264\n"
                    "stereo.fps=30\n"
                    "allow_missing_devices=true\n");
    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268\n"
                    "stereo.pid=0x1218\n"
                    "stereo.resolution=3200x1200\n"
                    "stereo.format=MJPEG\n"
                    "stereo.fps=30\n"
                    "allow_missing_devices=true\n");
    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268\n"
                    "stereo.pid=0x1218\n"
                    "stereo.resolution=3200x1200\n"
                    "stereo.format=H264\n"
                    "stereo.fps=60\n"
                    "allow_missing_devices=true\n");
    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268\n"
                    "stereo.pid=0x1218\n"
                    "stereo.format=H264\n"
                    "stereo.fps=30\n"
                    "allow_missing_devices=true\n");
    assert_rejected(product_config, camera_map,
                    "[cherry]\n"
                    "stereo.vid=0x5268\n"
                    "stereo.vid=0x5268\n"
                    "stereo.pid=0x1218\n"
                    "stereo.resolution=3200x1200\n"
                    "stereo.format=H264\n"
                    "stereo.fps=30\n"
                    "allow_missing_devices=true\n");

    std::filesystem::remove_all(directory);
    return 0;
}
