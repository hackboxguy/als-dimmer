#include "als-dimmer/interfaces.hpp"
#include "als-dimmer/logger.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>

namespace als_dimmer {

/**
 * File-based sensor for simulation and testing
 * Reads lux value from a text file
 */
class FileSensor : public SensorInterface {
public:
    explicit FileSensor(const std::string& file_path)
        : file_path_(file_path), last_lux_(0.0f), healthy_(false) {}

    bool init() override {
        LOG_DEBUG("FileSensor", "Initializing with file: " << file_path_);

        // Try to read the file to verify it exists and is accessible
        std::ifstream file(file_path_);
        if (!file.is_open()) {
            LOG_WARN("FileSensor", "Cannot open file (will retry on read)");
            // Don't fail init - file might be created later
        }

        healthy_ = true;
        return true;
    }

    float readLux() override {
        std::ifstream file(file_path_);
        if (!file.is_open()) {
            LOG_ERROR("FileSensor", "Cannot open file: " << file_path_);
            healthy_ = false;
            return -1.0f;
        }

        std::string line;
        if (!std::getline(file, line)) {
            LOG_ERROR("FileSensor", "Cannot read from file");
            healthy_ = false;
            return -1.0f;
        }

        try {
            float lux = std::stof(line);

            // Validate lux value
            if (lux < 0.0f) {
                LOG_WARN("FileSensor", "Negative lux value, clamping to 0");
                lux = 0.0f;
            }

            last_lux_ = lux;
            healthy_ = true;
            return lux;
        } catch (const std::exception& e) {
            LOG_ERROR("FileSensor", "Error parsing lux value: " << e.what());
            healthy_ = false;
            return -1.0f;
        }
    }

    bool isHealthy() const override {
        return healthy_;
    }

    std::string getType() const override {
        return "file";
    }

private:
    std::string file_path_;
    float last_lux_;
    bool healthy_;
};

// Factory function
std::unique_ptr<SensorInterface> createFileSensor(const std::string& file_path) {
    return std::make_unique<FileSensor>(file_path);
}

} // namespace als_dimmer
