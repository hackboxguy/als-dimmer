#ifndef ALS_DIMMER_CONFIG_HPP
#define ALS_DIMMER_CONFIG_HPP

#include <string>
#include <vector>
#include <stdexcept>

namespace als_dimmer {

// Configuration structures matching JSON schema

struct SensorConfig {
    std::string type;           // opti4001 | veml7700 | can_als | fpga_opti4001 | custom_i2c | file
    std::string device;         // For I2C sensors
    std::string address;        // For I2C sensors (hex string)
    std::string file_path;      // For file sensor

    // CAN-specific fields
    std::string can_interface;  // e.g., "can0"
    std::string can_id;         // e.g., "0x0A2"
    int timeout_ms = 5000;      // Timeout for considering data stale
};

struct OutputConfig {
    std::string type;           // ddcutil | dimmer200 | dimmer800 | custom_i2c | can | file
    std::string device;         // For I2C/ddcutil
    int display_number = 0;     // For ddcutil
    std::string address;        // For custom I2C, dimmer200, dimmer800
    std::string file_path;      // For file output

    // Range configuration
    std::vector<int> value_range = {0, 100};      // Device's native range
    std::vector<int> internal_range = {0, 100};   // Internal range (always 0-100)
};

struct StepSizes {
    int large = 10;
    int medium = 4;
    int small = 2;
};

struct ErrorThresholds {
    int large = 30;
    int small = 10;
};

struct Zone {
    std::string name;
    std::vector<float> lux_range;           // [min, max]
    std::vector<int> brightness_range;      // [min, max]
    std::string curve = "linear";           // linear | logarithmic
    StepSizes step_sizes;
    ErrorThresholds error_thresholds;
};

struct TcpSocketConfig {
    bool enabled = true;
    std::string listen_address = "127.0.0.1";
    int listen_port = 9000;
};

struct UnixSocketConfig {
    bool enabled = true;
    std::string path = "/tmp/als-dimmer.sock";
    std::string permissions = "0660";
    std::string owner = "root";
    std::string group = "root";
};

struct ControlConfig {
    // Socket configuration
    TcpSocketConfig tcp_socket;
    UnixSocketConfig unix_socket;

    // Legacy fields (for backward compatibility with old configs)
    std::string listen_address = "127.0.0.1";
    int listen_port = 9000;

    // Control settings
    int update_interval_ms = 500;
    int sensor_error_timeout_sec = 300;
    int fallback_brightness = 50;
    std::string state_file = "/var/lib/als-dimmer/state.json";
    int auto_resume_timeout_sec = 60;
    std::string log_level = "info";  // trace | debug | info | warn | error
};

struct CalibrationConfig {
    bool enabled = false;
    int sample_duration_sec = 60;
    bool auto_adjust_zones = true;
};

struct Config {
    SensorConfig sensor;
    OutputConfig output;
    ControlConfig control;
    std::vector<Zone> zones;
    CalibrationConfig calibration;

    // Load configuration from JSON file
    static Config loadFromFile(const std::string& filename);

    // Validate configuration
    void validate() const;

private:
    void setDefaults();
};

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace als_dimmer

#endif // ALS_DIMMER_CONFIG_HPP
