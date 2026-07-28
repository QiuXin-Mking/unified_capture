#include "core/product_config.h"

#include <fstream>
#include <map>

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

}  // namespace

std::string_view product_profile_name(ProductProfile profile) {
    switch (profile) {
        case ProductProfile::mango:
            return "mango";
        case ProductProfile::banana:
            return "banana";
    }
    return "";
}

ProductConfigResult load_product_configuration(
    const std::string& product_config_path,
    const std::string& camera_map_path) {
    ProductProfile profile;
    ProductConfigResult result = load_product_profile(product_config_path, &profile);
    if (!result.error.empty()) {
        return result;
    }

    CameraMap map;
    result = load_camera_map(camera_map_path, &map);
    if (!result.error.empty()) {
        return result;
    }

    ProductConfiguration configuration;
    configuration.profile = profile;
    if (profile == ProductProfile::banana) {
        const auto banana = map.find("banana");
        if (banana == map.end()) {
            return error_result("missing camera-map section: banana");
        }

        std::string allow_missing_devices;
        result = required_value(banana->second, "allow_missing_devices",
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

        result = required_value(banana->second, "wrist_left.product",
                                &configuration.wrist.left_product);
        if (!result.error.empty()) {
            return result;
        }
        result = required_value(banana->second, "wrist_right.product",
                                &configuration.wrist.right_product);
        if (!result.error.empty()) {
            return result;
        }

        // sixcam (optional, default false)
        auto sixcam = banana->second.find("sixcam.enabled");
        if (sixcam != banana->second.end()) {
            if (sixcam->second == "true") {
                configuration.sixcam_enabled = true;
            } else if (sixcam->second != "false") {
                return error_result("sixcam.enabled must be true or false");
            }
        }
    }

    return ProductConfigResult{configuration, ""};
}
