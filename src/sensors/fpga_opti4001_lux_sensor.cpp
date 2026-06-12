#include "als-dimmer/interfaces.hpp"
#include <iostream>
#include <memory>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {

/**
 * FPGA OPT4001 Ambient Light Sensor, fixed-RTL lux contract
 *
 * Architecture: FPGA acts as I2C slave to Pi (address 0x1D), and as I2C master
 * to OPT4001 sensor. FPGA maintains a cache of the latest decoded lux reading.
 *
 * I2C Protocol:
 * - Address: 0x1D (configurable)
 * - Write command: 0x00 0x00 0x00 0x0C (4 bytes, fixed)
 * - Read response: 4 bytes, big-endian uint32 integer lux
 *
 * Conversion: lux = response, scale factor 1.0
 *
 * Error Detection: 0xFFFFFFFF indicates FPGA/sensor failure or not-ready.
 */
class FPGAOpti4001LuxSensor : public SensorInterface {
public:
    FPGAOpti4001LuxSensor(const std::string& device, uint8_t address)
        : device_(device)
        , address_(address)
        , i2c_fd_(-1)
        , healthy_(false)
    {}

    ~FPGAOpti4001LuxSensor() {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[FPGA_OPT4001_LUX] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec << "\n";

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed to open I2C device: "
                      << strerror(errno) << "\n";
            return false;
        }

        if (ioctl(i2c_fd_, I2C_SLAVE, address_) < 0) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed to set I2C slave address: "
                      << strerror(errno) << "\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        uint32_t initial_lux = 0;
        if (!readLuxRegister(initial_lux)) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed initial I2C read test\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        if (initial_lux == INVALID_LUX) {
            std::cerr << "[FPGA_OPT4001_LUX] FPGA returned not-ready sentinel "
                      << "(0xFFFFFFFF); starting and waiting for a valid sample\n";
            healthy_ = false;
            return true;
        }

        healthy_ = true;
        std::cout << "[FPGA_OPT4001_LUX] Initialized successfully, initial reading: "
                  << initial_lux << " lux\n";
        return true;
    }

    float readLux() override {
        if (i2c_fd_ < 0) {
            std::cerr << "[FPGA_OPT4001_LUX] Sensor not initialized\n";
            healthy_ = false;
            return -1.0f;
        }

        uint32_t lux_value = 0;
        if (!readLuxRegister(lux_value)) {
            healthy_ = false;
            return -1.0f;
        }

        if (lux_value == INVALID_LUX) {
            std::cerr << "[FPGA_OPT4001_LUX] FPGA reported sensor failed/not-ready "
                      << "(0xFFFFFFFF)\n";
            healthy_ = false;
            return -1.0f;
        }

        const float lux = static_cast<float>(lux_value);

        static int debug_count = 0;
        if (debug_count++ < 10) {
            std::cout << "[FPGA_OPT4001_LUX] Lux u32: " << lux_value
                      << " -> Lux: " << lux << "\n";
        }

        if (lux > 120000.0f) {
            std::cerr << "[FPGA_OPT4001_LUX] WARNING: lux out of expected SOT-5X3 range: "
                      << lux << "\n";
        }

        healthy_ = true;
        return lux;
    }

    bool isHealthy() const override {
        return healthy_;
    }

    std::string getType() const override {
        return "fpga_opti4001_lux";
    }

private:
    bool readLuxRegister(uint32_t& value) {
        uint8_t cmd[4] = {0x00, 0x00, 0x00, 0x0C};
        if (write(i2c_fd_, cmd, 4) != 4) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed to write command: "
                      << strerror(errno) << "\n";
            return false;
        }

        uint8_t buf[4];
        if (read(i2c_fd_, buf, 4) != 4) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed to read response: "
                      << strerror(errno) << "\n";
            return false;
        }

        value = (static_cast<uint32_t>(buf[0]) << 24) |
                (static_cast<uint32_t>(buf[1]) << 16) |
                (static_cast<uint32_t>(buf[2]) << 8) |
                static_cast<uint32_t>(buf[3]);
        return true;
    }

    static const uint32_t INVALID_LUX = 0xFFFFFFFFu;

    std::string device_;
    uint8_t address_;
    int i2c_fd_;
    bool healthy_;
};

std::unique_ptr<SensorInterface> createFPGAOpti4001LuxSensor(
    const std::string& device,
    const std::string& address_str)
{
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::make_unique<FPGAOpti4001LuxSensor>(device, address);
}

} // namespace als_dimmer
