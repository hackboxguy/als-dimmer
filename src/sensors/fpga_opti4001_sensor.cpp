#include "als-dimmer/interfaces.hpp"
#include <iostream>
#include <memory>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {

/**
 * FPGA-based OPT4001 Ambient Light Sensor
 *
 * Architecture: FPGA acts as I2C slave to Pi (address 0x1D), and as I2C master
 * to OPT4001 sensor. FPGA maintains a cache of the latest lux reading.
 *
 * I2C Protocol:
 * - Address: 0x1D (configurable)
 * - Write command: 0x00 0x00 0x00 0x0C (4 bytes, fixed)
 * - Read response: 4 bytes
 *   - Byte 0: Reserved (ignore)
 *   - Bytes 1-3: 24-bit lux value (big-endian)
 *
 * Conversion: lux = raw_24bit * 0.64
 *
 * Error Detection: 0xFFFFFFFF indicates FPGA or sensor failure
 */
class FPGAOpti4001Sensor : public SensorInterface {
public:
    FPGAOpti4001Sensor(const std::string& device, uint8_t address)
        : device_(device), address_(address), i2c_fd_(-1), healthy_(false) {}

    ~FPGAOpti4001Sensor() {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[FPGA_OPT4001] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec << "\n";

        // Open I2C device
        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[FPGA_OPT4001] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // Set I2C slave address
        if (ioctl(i2c_fd_, I2C_SLAVE, address_) < 0) {
            std::cerr << "[FPGA_OPT4001] Failed to set I2C slave address: " << strerror(errno) << "\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        // Test read to verify FPGA is responding
        float test_lux = readLux();
        if (test_lux < 0.0f) {
            std::cerr << "[FPGA_OPT4001] Failed initial read test\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        std::cout << "[FPGA_OPT4001] Initialized successfully, initial reading: "
                  << test_lux << " lux\n";

        healthy_ = true;
        return true;
    }

    float readLux() override {
        if (i2c_fd_ < 0) {
            std::cerr << "[FPGA_OPT4001] Sensor not initialized\n";
            healthy_ = false;
            return -1.0f;
        }

        // Write 4-byte command: 0x00 0x00 0x00 0x0C
        uint8_t cmd[4] = {0x00, 0x00, 0x00, 0x0C};
        if (write(i2c_fd_, cmd, 4) != 4) {
            std::cerr << "[FPGA_OPT4001] Failed to write command: " << strerror(errno) << "\n";
            healthy_ = false;
            return -1.0f;
        }

        // Read 4-byte response
        uint8_t buf[4];
        if (read(i2c_fd_, buf, 4) != 4) {
            std::cerr << "[FPGA_OPT4001] Failed to read response: " << strerror(errno) << "\n";
            healthy_ = false;
            return -1.0f;
        }

        // Check for error condition (all bytes 0xFF)
        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF) {
            std::cerr << "[FPGA_OPT4001] FPGA reported error (0xFFFFFFFF)\n";
            healthy_ = false;
            return -1.0f;
        }

        // Extract 24-bit lux value (big-endian, ignore byte 0)
        // Byte 0: Reserved (ignore)
        // Byte 1: MSB
        // Byte 2: Middle
        // Byte 3: LSB
        uint32_t raw_value = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];

        // Convert to lux: raw * 0.64
        float lux = raw_value * 0.64f;

        // Debug output (first 10 readings)
        static int debug_count = 0;
        if (debug_count++ < 10) {
            std::cout << "[FPGA_OPT4001] Raw bytes: 0x" << std::hex
                      << (int)buf[0] << " 0x" << (int)buf[1]
                      << " 0x" << (int)buf[2] << " 0x" << (int)buf[3] << std::dec << "\n";
            std::cout << "[FPGA_OPT4001] Raw value: " << raw_value
                      << " -> Lux: " << lux << "\n";
        }

        // Sanity check (max ~117.4k lux for SOT-5X3 variant)
        // Max = (2^20 - 1) * 2^8 * 437.5e-6 = 118,362 lux theoretical
        // FPGA raw max = 117,441 / 0.64 = 183,501, allowing 120k lux for margin
        if (lux > 120000.0f) {
            std::cerr << "[FPGA_OPT4001] WARNING: lux out of expected range: " << lux << "\n";
            // Don't fail, just warn - might be valid in extreme conditions
        }

        healthy_ = true;
        return lux;
    }

    bool isHealthy() const override {
        return healthy_;
    }

    std::string getType() const override {
        return "fpga_opti4001";
    }

private:
    std::string device_;
    uint8_t address_;
    int i2c_fd_;
    bool healthy_;
};

// Factory function
std::unique_ptr<SensorInterface> createFPGAOpti4001Sensor(const std::string& device, const std::string& address_str) {
    // Parse hex address string (e.g., "0x1D")
    uint8_t address = std::stoi(address_str, nullptr, 16);
    return std::make_unique<FPGAOpti4001Sensor>(device, address);
}

} // namespace als_dimmer
