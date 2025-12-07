#include "als-dimmer/interfaces.hpp"
#include "als-dimmer/logger.hpp"
#include <fstream>
#include <sstream>
#include <memory>
#include <chrono>

namespace als_dimmer {

/**
 * FPGA OPTI4001 Sysfs Sensor
 *
 * Reads ambient light sensor values from the FPGA sysfs driver.
 * The sysfs node provides raw 24-bit values that are converted to lux
 * using the configurable scale_factor (default: 0.64).
 *
 * Sysfs path example: /sys/class/harman_9090_fpga_ctrl/display_ivi/light_sensor_read
 * Raw value range: 0 - 16777215
 * Lux conversion: lux = raw_value * scale_factor
 */
class FPGAOpti4001SysfsSensor : public SensorInterface {
public:
    FPGAOpti4001SysfsSensor(const std::string& sysfs_path, float scale_factor)
        : sysfs_path_(sysfs_path)
        , scale_factor_(scale_factor)
        , last_lux_(0.0f)
        , healthy_(false)
        , consecutive_errors_(0)
        , last_read_time_(std::chrono::steady_clock::now())
    {}

    bool init() override {
        LOG_DEBUG("FPGAOpti4001Sysfs", "Initializing with sysfs path: " << sysfs_path_);
        LOG_DEBUG("FPGAOpti4001Sysfs", "Scale factor: " << scale_factor_);

        // Try initial read to verify sysfs node exists and is accessible
        std::ifstream file(sysfs_path_);
        if (!file.is_open()) {
            LOG_WARN("FPGAOpti4001Sysfs", "Cannot open sysfs node (driver may not be loaded yet): " << sysfs_path_);
            // Don't fail init - driver might be loaded later
            healthy_ = false;
            return true;
        }

        // Try to read a value to verify the node is functional
        std::string line;
        if (std::getline(file, line)) {
            try {
                unsigned long raw_value = std::stoul(line);
                last_lux_ = static_cast<float>(raw_value) * scale_factor_;
                healthy_ = true;
                LOG_INFO("FPGAOpti4001Sysfs", "Initial read successful: raw=" << raw_value
                         << " lux=" << last_lux_);
            } catch (const std::exception& e) {
                LOG_WARN("FPGAOpti4001Sysfs", "Initial read parse error: " << e.what());
                healthy_ = false;
            }
        }

        return true;
    }

    float readLux() override {
        std::ifstream file(sysfs_path_);
        if (!file.is_open()) {
            LOG_ERROR("FPGAOpti4001Sysfs", "Cannot open sysfs node: " << sysfs_path_);
            handleError();
            return -1.0f;
        }

        std::string line;
        if (!std::getline(file, line)) {
            LOG_ERROR("FPGAOpti4001Sysfs", "Cannot read from sysfs node");
            handleError();
            return -1.0f;
        }

        // Trim whitespace (sysfs often adds newline)
        line.erase(line.find_last_not_of(" \t\n\r\f\v") + 1);

        try {
            unsigned long raw_value = std::stoul(line);

            // Validate raw value (24-bit max)
            if (raw_value > 16777215) {
                LOG_WARN("FPGAOpti4001Sysfs", "Raw value exceeds 24-bit max: " << raw_value);
                raw_value = 16777215;
            }

            // Convert raw to lux using scale factor
            float lux = static_cast<float>(raw_value) * scale_factor_;

            // Sanity check - lux should be non-negative
            if (lux < 0.0f) {
                LOG_WARN("FPGAOpti4001Sysfs", "Negative lux calculated, clamping to 0");
                lux = 0.0f;
            }

            last_lux_ = lux;
            healthy_ = true;
            consecutive_errors_ = 0;
            last_read_time_ = std::chrono::steady_clock::now();

            LOG_TRACE("FPGAOpti4001Sysfs", "Read: raw=" << raw_value << " lux=" << lux);

            return lux;

        } catch (const std::exception& e) {
            LOG_ERROR("FPGAOpti4001Sysfs", "Error parsing raw value '" << line << "': " << e.what());
            handleError();
            return -1.0f;
        }
    }

    bool isHealthy() const override {
        // Consider unhealthy if too many consecutive errors or stale data
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS) {
            return false;
        }

        // Check for stale data (no successful read in last 10 seconds)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_read_time_).count();
        if (elapsed > STALE_DATA_TIMEOUT_SEC) {
            return false;
        }

        return healthy_;
    }

    std::string getType() const override {
        return "fpga_opti4001_sysfs";
    }

private:
    void handleError() {
        consecutive_errors_++;
        if (consecutive_errors_ >= MAX_CONSECUTIVE_ERRORS) {
            healthy_ = false;
            LOG_WARN("FPGAOpti4001Sysfs", "Sensor marked unhealthy after "
                     << consecutive_errors_ << " consecutive errors");
        }
    }

    static constexpr int MAX_CONSECUTIVE_ERRORS = 5;
    static constexpr int STALE_DATA_TIMEOUT_SEC = 10;

    std::string sysfs_path_;
    float scale_factor_;
    float last_lux_;
    bool healthy_;
    int consecutive_errors_;
    std::chrono::steady_clock::time_point last_read_time_;
};

// Factory function
std::unique_ptr<SensorInterface> createFPGAOpti4001SysfsSensor(
    const std::string& sysfs_path,
    float scale_factor)
{
    return std::make_unique<FPGAOpti4001SysfsSensor>(sysfs_path, scale_factor);
}

} // namespace als_dimmer
