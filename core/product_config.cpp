#include "core/product_config.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>

namespace {

std::string trim_ascii_whitespace(const std::string& value) {
    const std::string whitespace = " \t\n\r\f\v";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

ProductConfigResult error_result(const std::string& error) {
    return ProductConfigResult{std::nullopt, error};
}

ProductConfigResult load_product_profile(const std::string& path,
                                         ProductProfile* profile) {
    std::ifstream input(path);
    if (!input) {
        return error_result("cannot open product configuration: " + path);
    }

    bool found_product = false;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim_ascii_whitespace(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos ||
            trimmed.find('=', equals + 1) != std::string::npos) {
            return error_result("invalid product configuration entry");
        }
        const std::string key = trim_ascii_whitespace(trimmed.substr(0, equals));
        const std::string value = trim_ascii_whitespace(trimmed.substr(equals + 1));
        if (key != "product" || value.empty()) {
            return error_result("invalid product configuration entry");
        }
        if (found_product) {
            return error_result("duplicate product entry");
        }
        found_product = true;

        if (value == "mango") {
            *profile = ProductProfile::mango;
        } else if (value == "banana") {
            *profile = ProductProfile::banana;
        } else if (value == "cherry") {
            *profile = ProductProfile::cherry;
        } else {
            return error_result("unknown product: " + value);
        }
    }

    if (!found_product) {
        return error_result("missing product entry");
    }
    if (input.bad()) {
        return error_result("failed reading product configuration: " + path);
    }
    return ProductConfigResult{};
}

using SectionEntries = std::map<std::string, std::string>;
using CameraMap = std::map<std::string, SectionEntries>;

ProductConfigResult load_camera_map(const std::string& path, CameraMap* map) {
    std::ifstream input(path);
    if (!input) {
        return error_result("cannot open camera map: " + path);
    }

    std::string current_section;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim_ascii_whitespace(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        if (trimmed.front() == '[') {
            if (trimmed.size() < 3 || trimmed.back() != ']') {
                return error_result("invalid camera-map section");
            }
            current_section = trim_ascii_whitespace(
                trimmed.substr(1, trimmed.size() - 2));
            if (current_section.empty() || map->contains(current_section)) {
                return error_result("duplicate or invalid camera-map section");
            }
            map->emplace(current_section, SectionEntries{});
            continue;
        }

        const std::size_t equals = trimmed.find('=');
        if (current_section.empty() || equals == std::string::npos ||
            trimmed.find('=', equals + 1) != std::string::npos) {
            return error_result("invalid camera-map entry");
        }
        const std::string key = trim_ascii_whitespace(trimmed.substr(0, equals));
        const std::string value = trim_ascii_whitespace(trimmed.substr(equals + 1));
        if (key.empty() || value.empty()) {
            return error_result("invalid camera-map entry");
        }
        const auto [_, inserted] = map->at(current_section).emplace(key, value);
        if (!inserted) {
            return error_result("duplicate camera-map key: " + key);
        }
    }

    if (input.bad()) {
        return error_result("failed reading camera map: " + path);
    }
    return ProductConfigResult{};
}

ProductConfigResult required_value(const SectionEntries& entries,
                                   const std::string& key,
                                   std::string* value) {
    const auto found = entries.find(key);
    if (found == entries.end()) {
        return error_result("missing required camera-map key: " + key);
    }
    *value = found->second;
    return ProductConfigResult{};
}

ProductConfigResult parse_hex_u16(const std::string& value, uint16_t* parsed) {
    if (value.size() <= 2 || value[0] != '0' || value[1] != 'x') {
        return error_result("expected hexadecimal value with 0x prefix");
    }

    unsigned int number = 0;
    const char* begin = value.data() + 2;
    const char* end = value.data() + value.size();
    const auto [next, error] = std::from_chars(begin, end, number, 16);
    if (error != std::errc{} || next != end ||
        number > std::numeric_limits<uint16_t>::max()) {
        return error_result("invalid 16-bit hexadecimal value");
    }
    *parsed = static_cast<uint16_t>(number);
    return ProductConfigResult{};
}

ProductConfigResult parse_decimal_int(const std::string& value, int* parsed) {
    if (value.empty()) {
        return error_result("invalid decimal value");
    }
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return error_result("invalid decimal value");
        }
    }

    int number = 0;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const auto [next, error] = std::from_chars(begin, end, number, 10);
    if (error != std::errc{} || next != end) {
        return error_result("decimal value is out of range");
    }
    *parsed = number;
    return ProductConfigResult{};
}

ProductConfigResult parse_cherry_resolution(const std::string& value,
                                             int* width,
                                             int* height) {
    const std::size_t separator = value.find('x');
    if (separator == std::string::npos ||
        value.find('x', separator + 1) != std::string::npos) {
        return error_result("stereo.resolution must be WIDTHxHEIGHT");
    }

    ProductConfigResult result = parse_decimal_int(value.substr(0, separator), width);
    if (!result.error.empty()) {
        return result;
    }
    result = parse_decimal_int(value.substr(separator + 1), height);
    if (!result.error.empty()) {
        return result;
    }
    if (*width != 3200 || *height != 1200) {
        return error_result("cherry stereo.resolution must be 3200x1200");
    }
    return ProductConfigResult{};
}

ProductConfigResult validate_cherry_keys(const SectionEntries& entries) {
    static const std::map<std::string, bool> allowed_keys = {
        {"stereo.vid", true},
        {"stereo.pid", true},
        {"stereo.resolution", true},
        {"stereo.format", true},
        {"stereo.fps", true},
        {"allow_missing_devices", true},
        {"wrist_left.product", true},
        {"wrist_right.product", true},
    };
    for (const auto& [key, _] : entries) {
        if (!allowed_keys.contains(key)) {
            return error_result("unknown cherry camera-map key: " + key);
        }
    }
    return ProductConfigResult{};
}

ProductConfigResult load_cherry_configuration(const SectionEntries& entries,
                                              CherryDeviceMap* cherry) {
    ProductConfigResult result = validate_cherry_keys(entries);
    if (!result.error.empty()) {
        return result;
    }

    std::string value;
    result = required_value(entries, "stereo.vid", &value);
    if (!result.error.empty()) {
        return result;
    }
    result = parse_hex_u16(value, &cherry->vid);
    if (!result.error.empty()) {
        return result;
    }
    if (cherry->vid != 0x5268) {
        return error_result("cherry stereo.vid must be 0x5268");
    }

    result = required_value(entries, "stereo.pid", &value);
    if (!result.error.empty()) {
        return result;
    }
    result = parse_hex_u16(value, &cherry->pid);
    if (!result.error.empty()) {
        return result;
    }
    if (cherry->pid != 0x1218) {
        return error_result("cherry stereo.pid must be 0x1218");
    }

    result = required_value(entries, "stereo.resolution", &value);
    if (!result.error.empty()) {
        return result;
    }
    result = parse_cherry_resolution(value, &cherry->width, &cherry->height);
    if (!result.error.empty()) {
        return result;
    }

    result = required_value(entries, "stereo.format", &cherry->format);
    if (!result.error.empty()) {
        return result;
    }
    if (cherry->format != "H264") {
        return error_result("cherry stereo.format must be H264");
    }

    result = required_value(entries, "stereo.fps", &value);
    if (!result.error.empty()) {
        return result;
    }
    result = parse_decimal_int(value, &cherry->fps);
    if (!result.error.empty()) {
        return result;
    }
    if (cherry->fps != 30) {
        return error_result("cherry stereo.fps must be 30");
    }

    result = required_value(entries, "allow_missing_devices", &value);
    if (!result.error.empty()) {
        return result;
    }
    if (value == "true") {
        cherry->allow_missing_devices = true;
    } else if (value == "false") {
        cherry->allow_missing_devices = false;
    } else {
        return error_result("allow_missing_devices must be true or false");
    }

    const auto left = entries.find("wrist_left.product");
    if (left != entries.end()) {
        cherry->wrist_left_product = left->second;
    }
    const auto right = entries.find("wrist_right.product");
    if (right != entries.end()) {
        cherry->wrist_right_product = right->second;
    }
    return ProductConfigResult{};
}

}  // namespace

std::string_view product_profile_name(ProductProfile profile) {
    switch (profile) {
        case ProductProfile::mango:
            return "mango";
        case ProductProfile::banana:
            return "banana";
        case ProductProfile::cherry:
            return "cherry";
    }
    return "";
}

ProductConfigResult load_product_configuration_for_profile(
    ProductProfile profile,
    const std::string& camera_map_path) {
    CameraMap map;
    ProductConfigResult result = load_camera_map(camera_map_path, &map);
    if (!result.error.empty()) {
        return result;
    }

    ProductConfiguration configuration;
    configuration.profile = profile;
    if (profile == ProductProfile::mango) {
        const auto mango = map.find("mango");
        if (mango == map.end()) {
            return error_result("missing camera-map section: mango");
        }

        std::string allow_missing_devices;
        result = required_value(mango->second, "allow_missing_devices",
                                &allow_missing_devices);
        if (!result.error.empty()) {
            return result;
        }
        if (allow_missing_devices == "true") {
            configuration.wrist.allow_missing_devices = true;
        } else if (allow_missing_devices == "false") {
            configuration.wrist.allow_missing_devices = false;
        } else {
            return error_result("allow_missing_devices must be true or false");
        }

        result = required_value(mango->second, "wrist_left.product",
                                &configuration.wrist.left_product);
        if (!result.error.empty()) {
            return result;
        }
        result = required_value(mango->second, "wrist_right.product",
                                &configuration.wrist.right_product);
        if (!result.error.empty()) {
            return result;
        }

        // sixcam (optional, default false)
        auto sixcam = mango->second.find("sixcam.enabled");
        if (sixcam != mango->second.end()) {
            if (sixcam->second == "true") {
                configuration.sixcam_enabled = true;
            } else if (sixcam->second != "false") {
                return error_result("sixcam.enabled must be true or false");
            }
        }
    } else if (profile == ProductProfile::cherry) {
        const auto cherry = map.find("cherry");
        if (cherry == map.end()) {
            return error_result("missing camera-map section: cherry");
        }
        result = load_cherry_configuration(cherry->second, &configuration.cherry);
        if (!result.error.empty()) {
            return result;
        }
    }

    return ProductConfigResult{configuration, ""};
}

ProductConfigResult load_product_configuration(
    const std::string& product_config_path,
    const std::string& camera_map_path) {
    ProductProfile profile;
    ProductConfigResult result = load_product_profile(product_config_path, &profile);
    if (!result.error.empty()) {
        return result;
    }
    return load_product_configuration_for_profile(profile, camera_map_path);
}

std::optional<ProductProfile> parse_product_profile(std::string_view value) {
    if (value == "mango") {
        return ProductProfile::mango;
    }
    if (value == "banana") {
        return ProductProfile::banana;
    }
    if (value == "cherry") {
        return ProductProfile::cherry;
    }
    return std::nullopt;
}

bool write_product_profile(const std::string& path, ProductProfile profile) {
    const std::string temporary_path = path + ".tmp";
    {
        std::ofstream output(temporary_path, std::ios::trunc);
        if (!output) {
            return false;
        }
        output << "product=" << product_profile_name(profile) << '\n';
        if (!output) {
            return false;
        }
    }
    if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
        std::remove(temporary_path.c_str());
        return false;
    }
    return true;
}
