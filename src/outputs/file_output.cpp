#include "als-dimmer/interfaces.hpp"
#include "als-dimmer/logger.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>

namespace als_dimmer {

/**
 * File-based output for simulation and testing
 * Writes brightness value to a text file
 */
class FileOutput : public OutputInterface {
public:
    explicit FileOutput(const std::string& file_path)
        : file_path_(file_path), current_brightness_(0) {}

    bool init() override {
        LOG_DEBUG("FileOutput", "Initializing with file: " << file_path_);

        // Try to write initial value to verify file is writable
        if (!writeBrightnessToFile(0)) {
            LOG_ERROR("FileOutput", "Cannot write to file: " << file_path_);
            return false;
        }

        current_brightness_ = 0;
        LOG_DEBUG("FileOutput", "Initialized successfully");
        return true;
    }

    bool setBrightness(int brightness) override {
        // Clamp brightness to valid range
        if (brightness < 0) {
            brightness = 0;
        } else if (brightness > 100) {
            brightness = 100;
        }

        if (!writeBrightnessToFile(brightness)) {
            LOG_ERROR("FileOutput", "Failed to write brightness");
            return false;
        }

        current_brightness_ = brightness;
        return true;
    }

    int getCurrentBrightness() override {
        return current_brightness_;
    }

    std::string getType() const override {
        return "file";
    }

private:
    bool writeBrightnessToFile(int brightness) {
        std::ofstream file(file_path_);
        if (!file.is_open()) {
            return false;
        }

        file << brightness << "\n";
        return file.good();
    }

    std::string file_path_;
    int current_brightness_;
};

// Factory function
std::unique_ptr<OutputInterface> createFileOutput(const std::string& file_path) {
    return std::make_unique<FileOutput>(file_path);
}

} // namespace als_dimmer
