#include "als-dimmer/interfaces.hpp"
#include "als-dimmer/logger.hpp"
#include <fstream>
#include <sstream>
#include <memory>

namespace als_dimmer {

/**
 * FPGA Sysfs Dimmer Output
 *
 * Controls display brightness via the FPGA sysfs driver's global_dimming node.
 * Takes standard 0-100 brightness values and scales them to the hardware range
 * (default 0-800).
 *
 * Sysfs path example: /sys/class/harman_9090_fpga_ctrl/display_ivi/global_dimming
 * Hardware range: 0-800 (configurable via max_value)
 * Input range: 0-100 (standard als-dimmer interface)
 *
 * The driver handles BCD encoding internally, so we just write decimal values.
 */
class FPGASysfsOutput : public OutputInterface {
public:
    FPGASysfsOutput(const std::string& sysfs_path, int max_value)
        : sysfs_path_(sysfs_path)
        , max_value_(max_value)
        , current_brightness_(0)
        , last_hw_value_(0)
    {}

    bool init() override {
        LOG_DEBUG("FPGASysfsOutput", "Initializing with sysfs path: " << sysfs_path_);
        LOG_DEBUG("FPGASysfsOutput", "Max hardware value: " << max_value_);

        // Verify the sysfs node is writable by attempting to read current value
        // Note: global_dimming is a cached value, so reading should work
        std::ifstream read_file(sysfs_path_);
        if (!read_file.is_open()) {
            LOG_ERROR("FPGASysfsOutput", "Cannot open sysfs node for reading: " << sysfs_path_);
            LOG_ERROR("FPGASysfsOutput", "Ensure FPGA driver is loaded");
            return false;
        }

        std::string line;
        if (std::getline(read_file, line)) {
            try {
                int hw_value = std::stoi(line);
                // Convert hardware value back to 0-100 range
                current_brightness_ = (hw_value * 100) / max_value_;
                last_hw_value_ = hw_value;
                LOG_INFO("FPGASysfsOutput", "Initial hardware value: " << hw_value
                         << " (brightness: " << current_brightness_ << "%)");
            } catch (const std::exception& e) {
                LOG_WARN("FPGASysfsOutput", "Could not parse initial value: " << e.what());
                current_brightness_ = 0;
                last_hw_value_ = 0;
            }
        }
        read_file.close();

        // Test write capability
        if (!writeToSysfs(last_hw_value_)) {
            LOG_ERROR("FPGASysfsOutput", "Cannot write to sysfs node (permission denied?)");
            return false;
        }

        LOG_DEBUG("FPGASysfsOutput", "Initialized successfully");
        return true;
    }

    bool setBrightness(int brightness) override {
        // Clamp brightness to valid range
        if (brightness < 0) {
            brightness = 0;
        } else if (brightness > 100) {
            brightness = 100;
        }

        // Scale 0-100 to 0-max_value
        int hw_value = (brightness * max_value_) / 100;

        // Avoid unnecessary writes if value hasn't changed
        if (hw_value == last_hw_value_ && brightness == current_brightness_) {
            LOG_TRACE("FPGASysfsOutput", "Skipping write - value unchanged: " << hw_value);
            return true;
        }

        if (!writeToSysfs(hw_value)) {
            LOG_ERROR("FPGASysfsOutput", "Failed to write brightness " << hw_value
                      << " to " << sysfs_path_);
            return false;
        }

        current_brightness_ = brightness;
        last_hw_value_ = hw_value;

        LOG_DEBUG("FPGASysfsOutput", "Set brightness: " << brightness << "% (hw: " << hw_value << ")");
        return true;
    }

    int getCurrentBrightness() override {
        return current_brightness_;
    }

    std::string getType() const override {
        return "fpga_sysfs_dimmer";
    }

private:
    bool writeToSysfs(int value) {
        std::ofstream file(sysfs_path_);
        if (!file.is_open()) {
            LOG_ERROR("FPGASysfsOutput", "Cannot open sysfs node for writing: " << sysfs_path_);
            return false;
        }

        file << value;

        if (!file.good()) {
            LOG_ERROR("FPGASysfsOutput", "Write failed to sysfs node");
            return false;
        }

        return true;
    }

    std::string sysfs_path_;
    int max_value_;
    int current_brightness_;
    int last_hw_value_;
};

// Factory function
std::unique_ptr<OutputInterface> createFPGASysfsOutput(
    const std::string& sysfs_path,
    int max_value)
{
    return std::make_unique<FPGASysfsOutput>(sysfs_path, max_value);
}

} // namespace als_dimmer
