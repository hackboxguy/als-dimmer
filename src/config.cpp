#include "als-dimmer/config.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace als_dimmer {

Config Config::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw ConfigError("Failed to open config file: " + filename);
    }

    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        throw ConfigError("JSON parse error: " + std::string(e.what()));
    }

    Config config;

    // Parse sensor configuration
    if (!j.contains("sensor")) {
        throw ConfigError("Missing required field: sensor");
    }
    auto& sensor_json = j["sensor"];

    if (!sensor_json.contains("type")) {
        throw ConfigError("Missing required field: sensor.type");
    }
    config.sensor.type = sensor_json["type"].get<std::string>();

    if (sensor_json.contains("device")) {
        config.sensor.device = sensor_json["device"].get<std::string>();
    }
    if (sensor_json.contains("address")) {
        config.sensor.address = sensor_json["address"].get<std::string>();
    }
    if (sensor_json.contains("file_path")) {
        config.sensor.file_path = sensor_json["file_path"].get<std::string>();
    }
    if (sensor_json.contains("can_interface")) {
        config.sensor.can_interface = sensor_json["can_interface"].get<std::string>();
    }
    if (sensor_json.contains("can_id")) {
        config.sensor.can_id = sensor_json["can_id"].get<std::string>();
    }

    // Parse output configuration
    if (!j.contains("output")) {
        throw ConfigError("Missing required field: output");
    }
    auto& output_json = j["output"];

    if (!output_json.contains("type")) {
        throw ConfigError("Missing required field: output.type");
    }
    config.output.type = output_json["type"].get<std::string>();

    if (output_json.contains("device")) {
        config.output.device = output_json["device"].get<std::string>();
    }
    if (output_json.contains("display_number")) {
        config.output.display_number = output_json["display_number"].get<int>();
    }
    if (output_json.contains("address")) {
        config.output.address = output_json["address"].get<std::string>();
    }
    if (output_json.contains("file_path")) {
        config.output.file_path = output_json["file_path"].get<std::string>();
    }
    if (output_json.contains("value_range")) {
        config.output.value_range = output_json["value_range"].get<std::vector<int>>();
    }
    if (output_json.contains("internal_range")) {
        config.output.internal_range = output_json["internal_range"].get<std::vector<int>>();
    }

    // Parse control configuration (with defaults)
    if (j.contains("control")) {
        auto& control_json = j["control"];

        if (control_json.contains("listen_address")) {
            config.control.listen_address = control_json["listen_address"].get<std::string>();
        }
        if (control_json.contains("listen_port")) {
            config.control.listen_port = control_json["listen_port"].get<int>();
        }
        if (control_json.contains("update_interval_ms")) {
            config.control.update_interval_ms = control_json["update_interval_ms"].get<int>();
        }
        if (control_json.contains("sensor_error_timeout_sec")) {
            config.control.sensor_error_timeout_sec = control_json["sensor_error_timeout_sec"].get<int>();
        }
        if (control_json.contains("fallback_brightness")) {
            config.control.fallback_brightness = control_json["fallback_brightness"].get<int>();
        }
        if (control_json.contains("state_file")) {
            config.control.state_file = control_json["state_file"].get<std::string>();
        }
        if (control_json.contains("auto_resume_timeout_sec")) {
            config.control.auto_resume_timeout_sec = control_json["auto_resume_timeout_sec"].get<int>();
        }
        if (control_json.contains("log_level")) {
            config.control.log_level = control_json["log_level"].get<std::string>();
        }
    }

    // Parse zones
    if (!j.contains("zones")) {
        throw ConfigError("Missing required field: zones");
    }
    if (!j["zones"].is_array() || j["zones"].empty()) {
        throw ConfigError("zones must be a non-empty array");
    }

    for (auto& zone_json : j["zones"]) {
        Zone zone;

        if (!zone_json.contains("name")) {
            throw ConfigError("Zone missing required field: name");
        }
        zone.name = zone_json["name"].get<std::string>();

        if (!zone_json.contains("lux_range")) {
            throw ConfigError("Zone '" + zone.name + "' missing required field: lux_range");
        }
        zone.lux_range = zone_json["lux_range"].get<std::vector<float>>();

        if (!zone_json.contains("brightness_range")) {
            throw ConfigError("Zone '" + zone.name + "' missing required field: brightness_range");
        }
        zone.brightness_range = zone_json["brightness_range"].get<std::vector<int>>();

        if (zone_json.contains("curve")) {
            zone.curve = zone_json["curve"].get<std::string>();
        }

        // Parse step sizes (with defaults)
        if (zone_json.contains("step_sizes")) {
            auto& step_json = zone_json["step_sizes"];
            if (step_json.contains("large")) {
                zone.step_sizes.large = step_json["large"].get<int>();
            }
            if (step_json.contains("medium")) {
                zone.step_sizes.medium = step_json["medium"].get<int>();
            }
            if (step_json.contains("small")) {
                zone.step_sizes.small = step_json["small"].get<int>();
            }
        }

        // Parse error thresholds (with defaults)
        if (zone_json.contains("error_thresholds")) {
            auto& threshold_json = zone_json["error_thresholds"];
            if (threshold_json.contains("large")) {
                zone.error_thresholds.large = threshold_json["large"].get<int>();
            }
            if (threshold_json.contains("small")) {
                zone.error_thresholds.small = threshold_json["small"].get<int>();
            }
        }

        config.zones.push_back(zone);
    }

    // Parse calibration configuration (optional)
    if (j.contains("calibration")) {
        auto& calib_json = j["calibration"];

        if (calib_json.contains("enabled")) {
            config.calibration.enabled = calib_json["enabled"].get<bool>();
        }
        if (calib_json.contains("sample_duration_sec")) {
            config.calibration.sample_duration_sec = calib_json["sample_duration_sec"].get<int>();
        }
        if (calib_json.contains("auto_adjust_zones")) {
            config.calibration.auto_adjust_zones = calib_json["auto_adjust_zones"].get<bool>();
        }
    }

    // Validate the loaded configuration
    config.validate();

    return config;
}

void Config::validate() const {
    // Validate sensor configuration
    if (sensor.type.empty()) {
        throw ConfigError("sensor.type cannot be empty");
    }

    // Type-specific validation
    if (sensor.type == "opti4001" || sensor.type == "veml7700" || sensor.type == "custom_i2c" || sensor.type == "fpga_opti4001") {
        if (sensor.device.empty()) {
            throw ConfigError("sensor.device is required for I2C sensor types");
        }
        if (sensor.address.empty()) {
            throw ConfigError("sensor.address is required for I2C sensor types");
        }
    } else if (sensor.type == "file") {
        if (sensor.file_path.empty()) {
            throw ConfigError("sensor.file_path is required for file sensor type");
        }
    } else if (sensor.type == "can") {
        if (sensor.can_interface.empty()) {
            throw ConfigError("sensor.can_interface is required for CAN sensor type");
        }
        if (sensor.can_id.empty()) {
            throw ConfigError("sensor.can_id is required for CAN sensor type");
        }
    } else {
        throw ConfigError("Unknown sensor type: " + sensor.type);
    }

    // Validate output configuration
    if (output.type.empty()) {
        throw ConfigError("output.type cannot be empty");
    }

    if (output.type == "ddcutil" || output.type == "custom_i2c" || output.type == "dimmer200" || output.type == "dimmer800") {
        if (output.device.empty()) {
            throw ConfigError("output.device is required for " + output.type + " output type");
        }
        if (output.type == "dimmer200" || output.type == "dimmer800" || output.type == "custom_i2c") {
            if (output.address.empty()) {
                throw ConfigError("output.address is required for " + output.type + " output type");
            }
        }
    } else if (output.type == "file") {
        if (output.file_path.empty()) {
            throw ConfigError("output.file_path is required for file output type");
        }
    } else if (output.type == "can") {
        // CAN output validation (Phase 3)
    } else {
        throw ConfigError("Unknown output type: " + output.type);
    }

    // Validate zones
    if (zones.empty()) {
        throw ConfigError("At least one zone must be defined");
    }

    for (const auto& zone : zones) {
        if (zone.lux_range.size() != 2) {
            throw ConfigError("Zone '" + zone.name + "' lux_range must have exactly 2 values [min, max]");
        }
        if (zone.lux_range[0] >= zone.lux_range[1]) {
            throw ConfigError("Zone '" + zone.name + "' lux_range min must be less than max");
        }
        if (zone.brightness_range.size() != 2) {
            throw ConfigError("Zone '" + zone.name + "' brightness_range must have exactly 2 values [min, max]");
        }
        if (zone.brightness_range[0] < 0 || zone.brightness_range[1] > 100) {
            throw ConfigError("Zone '" + zone.name + "' brightness_range must be within 0-100");
        }
        if (zone.curve != "linear" && zone.curve != "logarithmic") {
            throw ConfigError("Zone '" + zone.name + "' curve must be 'linear' or 'logarithmic'");
        }
    }

    // Validate control parameters
    if (control.listen_port < 1 || control.listen_port > 65535) {
        throw ConfigError("control.listen_port must be between 1 and 65535");
    }
    if (control.update_interval_ms < 100 || control.update_interval_ms > 10000) {
        throw ConfigError("control.update_interval_ms must be between 100 and 10000");
    }
    if (control.fallback_brightness < 0 || control.fallback_brightness > 100) {
        throw ConfigError("control.fallback_brightness must be between 0 and 100");
    }
}

} // namespace als_dimmer
