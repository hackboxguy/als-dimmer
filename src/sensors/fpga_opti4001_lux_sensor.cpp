#include "als-dimmer/interfaces.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
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

        // The FPGA's I2C slave corrupts reads that carry the byte 0x67 after the first byte
        // (pixelpipe-fpga docs/dynamic-local-dimming.md §11 N10, review v3 §3): at lux values
        // with a 0x67 byte (103, 359, 615, ... 26368..26623 ...) ~30 % of reads fail (EIO) or
        // come back shifted / with bit 7 dropped. So: one retry on EIO, implausible values
        // rejected, and the median of the last three good reads is returned.
        uint32_t lux_value = 0;
        if (!readLuxRegister(lux_value) && !readLuxRegister(lux_value)) {
            if (history_count_ > 0) return median();       // keep the last good estimate
            healthy_ = false;
            return -1.0f;
        }

        if (lux_value == INVALID_LUX) {
            std::cerr << "[FPGA_OPT4001_LUX] FPGA reported sensor failed/not-ready "
                      << "(0xFFFFFFFF)\n";
            healthy_ = false;
            return -1.0f;
        }

        const float raw = static_cast<float>(lux_value);
        if (raw > MAX_PLAUSIBLE_LUX) {
            if (rejected_++ % 50 == 0) {
                std::cerr << "[FPGA_OPT4001_LUX] Ignoring implausible lux " << lux_value
                          << " (read corruption?)\n";
            }
            if (history_count_ > 0) return median();
            healthy_ = false;
            return -1.0f;
        }
        history_[history_pos_] = raw;
        history_pos_ = (history_pos_ + 1) % 3;
        if (history_count_ < 3) ++history_count_;
        const float lux = median();

        static int debug_count = 0;
        if (debug_count++ < 10) {
            std::cout << "[FPGA_OPT4001_LUX] Lux u32: " << lux_value
                      << " -> Lux: " << lux << "\n";
        }

        healthy_ = true;
        return lux;
    }

    bool isHealthy() const override {
        return healthy_;
    }

    // Median of the last (up to) three good reads.
    float median() const {
        if (history_count_ == 1) return history_[0];
        if (history_count_ == 2) return (history_[0] + history_[1]) / 2.0f;
        float a = history_[0], b = history_[1], c = history_[2];
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    std::string getType() const override {
        return "fpga_opti4001_lux";
    }

private:
    // One I2C_RDWR transaction (pointer write + repeated-start read): with a separate write() and
    // read(), another process reading the FPGA in between (disp-settings, disptool) moves the
    // slave's pointer and the 4 bytes come from another register (same race as the wp_adjust
    // restore, 2026-10-14).
    bool readLuxRegister(uint32_t& value) {
        uint8_t cmd[4] = {0x00, 0x00, 0x00, 0x0C};
        uint8_t buf[4] = {0, 0, 0, 0};
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(cmd);
        msgs[0].buf = cmd;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = sizeof(buf);
        msgs[1].buf = buf;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;
        if (ioctl(i2c_fd_, I2C_RDWR, &xfer) < 0) {
            std::cerr << "[FPGA_OPT4001_LUX] Failed to read lux: " << strerror(errno) << "\n";
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
    static constexpr float MAX_PLAUSIBLE_LUX = 120000.0f;   // OPT4001 full scale is ~84k lux
    float history_[3] = {0.0f, 0.0f, 0.0f};
    int history_pos_ = 0;
    int history_count_ = 0;
    unsigned rejected_ = 0;
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
