#include "als-dimmer/outputs/i2c_dimmer_output.hpp"
#include <iostream>
#include <memory>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cstring>
#include <algorithm>

namespace als_dimmer {

I2CDimmerOutput::I2CDimmerOutput(const std::string& device, uint8_t address, DimmerType type)
    : device_(device)
    , address_(address)
    , type_(type)
    , fd_(-1)
    , current_brightness_(0) {

    // Set parameters based on dimmer type
    if (type_ == DimmerType::DIMMER_200) {
        max_native_brightness_ = 200;
        command_byte_ = 0x28;
    } else {  // DIMMER_800
        max_native_brightness_ = 800;
        command_byte_ = 0x35;
    }
}

I2CDimmerOutput::~I2CDimmerOutput() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

bool I2CDimmerOutput::init() {
    // Open I2C device
    fd_ = open(device_.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "[I2CDimmer]  Failed to open " << device_ << ": " << strerror(errno) << "\n";
        return false;
    }

    // Set I2C slave address
    if (ioctl(fd_, I2C_SLAVE, address_) < 0) {
        std::cerr << "[I2CDimmer]  Failed to set I2C slave address 0x"
                  << std::hex << static_cast<int>(address_) << std::dec
                  << ": " << strerror(errno) << "\n";
        close(fd_);
        fd_ = -1;
        return false;
    }

    std::cout << "[I2CDimmer]  Initialized on " << device_
              << " at address 0x" << std::hex << static_cast<int>(address_) << std::dec
              << " (type: " << (type_ == DimmerType::DIMMER_200 ? "dimmer200" : "dimmer800")
              << ", range: 0-" << max_native_brightness_ << ")\n";

    return true;
}

bool I2CDimmerOutput::setBrightness(int brightness) {
    // Clamp to valid range
    brightness = std::max(0, std::min(100, brightness));

    // Scale to native brightness
    int native_value = scaleToNative(brightness);

    // Write to I2C dimmer
    if (writeI2CBrightness(native_value)) {
        current_brightness_ = brightness;
        return true;
    }

    return false;
}

int I2CDimmerOutput::getCurrentBrightness() {
    // Return cached brightness (hardware doesn't support readback)
    return current_brightness_;
}

std::string I2CDimmerOutput::getType() const {
    return (type_ == DimmerType::DIMMER_200) ? "dimmer200" : "dimmer800";
}

bool I2CDimmerOutput::writeI2CBrightness(int native_value) {
    if (fd_ < 0) {
        std::cerr << "[I2CDimmer]  I2C device not initialized\n";
        return false;
    }

    // Clamp to native range
    native_value = std::max(0, std::min(max_native_brightness_, native_value));

    uint8_t buffer[7];  // Max size for dimmer800
    int buffer_len;

    // Build I2C command based on dimmer type
    // Common header: 0x00 0x00 0x00
    buffer[0] = 0x00;
    buffer[1] = 0x00;
    buffer[2] = 0x00;
    buffer[3] = command_byte_;

    if (type_ == DimmerType::DIMMER_200) {
        // dimmer200: 5 bytes total (header + command + 1-byte value)
        buffer[4] = static_cast<uint8_t>(native_value);
        buffer_len = 5;
    } else {
        // dimmer800: 6 bytes total (header + command + 2-byte big-endian value)
        buffer[4] = static_cast<uint8_t>((native_value >> 8) & 0xFF);  // High byte
        buffer[5] = static_cast<uint8_t>(native_value & 0xFF);          // Low byte
        buffer_len = 6;
    }

    // Write to I2C device
    ssize_t result = write(fd_, buffer, buffer_len);
    if (result != buffer_len) {
        std::cerr << "[I2CDimmer]  I2C write failed (wrote " << result << " of " << buffer_len
                  << " bytes): " << strerror(errno) << "\n";
        return false;
    }

    return true;
}

int I2CDimmerOutput::scaleToNative(int percent) const {
    // Scale 0-100% to native range (0-200 or 0-800)
    return static_cast<int>((percent / 100.0) * max_native_brightness_);
}

// Factory function
std::unique_ptr<OutputInterface> createI2CDimmerOutput(const std::string& device,
                                                        uint8_t address,
                                                        const std::string& type) {
    I2CDimmerOutput::DimmerType dimmer_type;

    if (type == "dimmer200") {
        dimmer_type = I2CDimmerOutput::DimmerType::DIMMER_200;
    } else if (type == "dimmer800") {
        dimmer_type = I2CDimmerOutput::DimmerType::DIMMER_800;
    } else {
        std::cerr << "[I2CDimmer]  Unknown dimmer type: " << type << "\n";
        return nullptr;
    }

    return std::make_unique<I2CDimmerOutput>(device, address, dimmer_type);
}

} // namespace als_dimmer
