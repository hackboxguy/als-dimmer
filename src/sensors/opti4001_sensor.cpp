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
 * OPT4001 Ambient Light Sensor
 * I2C address: 0x44 or 0x45
 *
 * Register map (all 16-bit registers):
 * 0x00: EXPONENT[15:12] + RESULT_MSB[11:0]
 * 0x01: RESULT_LSB[15:8] + COUNTER[7:4] + CRC[3:0]
 * 0x0A: Configuration register
 * 0x11: Device ID (DIDH = 0x121)
 */
class OPTI4001Sensor : public SensorInterface {
public:
    OPTI4001Sensor(const std::string& device, uint8_t address)
        : device_(device), address_(address), i2c_fd_(-1), healthy_(false) {}

    ~OPTI4001Sensor() {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[OPTI4001]  Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec << "\n";

        // Open I2C device
        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[OPTI4001]  Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // Set I2C slave address
        if (ioctl(i2c_fd_, I2C_SLAVE, address_) < 0) {
            std::cerr << "[OPTI4001]  Failed to set I2C slave address: " << strerror(errno) << "\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        // Read device ID (register 0x11, bits[11:0] should be 0x121 for OPT4001)
        uint16_t device_id = 0;
        if (!readRegister16(0x11, device_id)) {
            std::cerr << "[OPTI4001]  Failed to read device ID\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        uint16_t did_h = device_id & 0x0FFF;  // Extract DIDH[11:0]
        std::cout << "[OPTI4001]  Device ID: 0x" << std::hex << did_h << std::dec << "\n";
        // OPT4001 DIDH should be 0x121
        if (did_h != 0x121) {
            std::cerr << "[OPTI4001]  WARNING: Unexpected device ID (expected 0x121)\n";
        }

        // Configure sensor (Register 0x0A - Configuration register)
        // Bit [15]: QWAKE = 0 (normal operation)
        // Bit [14]: Reserved = 0 (MUST BE 0 per datasheet!)
        // Bits[13:10]: RANGE = 0xC (auto-range, hardware manages exponent 0-11 automatically)
        // Bits[9:6]: CONVERSION_TIME = 0x8 (100ms per datasheet - value 11=800ms, but ESP32 uses 8=100ms)
        // Bits[5:4]: OPERATING_MODE = 0x3 (continuous)
        // Bit [3]: LATCH = 1 (latch interrupts)
        // Bit [2]: INT_POL = 0 (active low)
        // Bits[1:0]: FAULT_COUNT = 0x1 (one fault)
        // Calculated: (0xC<<10) | (0x8<<6) | (0x3<<4) | (1<<3) | (0<<2) | 1 = 0x3239
        uint16_t config = 0x3239;  // Match working ESP32 config exactly

        if (!writeRegister16(0x0A, config)) {
            std::cerr << "[OPTI4001]  Failed to configure sensor\n";
            close(i2c_fd_);
            i2c_fd_ = -1;
            return false;
        }

        // Verify configuration was applied (readback register 0x0A)
        uint16_t readback_config = 0;
        if (!readRegister16(0x0A, readback_config)) {
            std::cerr << "[OPTI4001]  WARNING: Failed to read back configuration\n";
        } else {
            std::cout << "[OPTI4001]  Config written: 0x" << std::hex << config
                      << " readback: 0x" << readback_config << std::dec << "\n";
            if (readback_config != config) {
                std::cerr << "[OPTI4001]  WARNING: Configuration mismatch!\n";
            }
        }

        std::cout << "[OPTI4001]  Sensor configured (continuous mode, auto-range, 100ms conversion)\n";

        // Wait for first conversion (100ms + margin)
        // CRITICAL: Must wait full conversion time or auto-range won't initialize properly!
        usleep(150000);  // 150ms

        healthy_ = true;
        return true;
    }

    float readLux() override {
        if (i2c_fd_ < 0) {
            std::cerr << "[OPTI4001]  Sensor not initialized\n";
            healthy_ = false;
            return -1.0f;
        }

        // Read register 0x00: EXPONENT[15:12] + RESULT_MSB[11:0]
        uint16_t reg0 = 0;
        if (!readRegister16(0x00, reg0)) {
            std::cerr << "[OPTI4001]  Failed to read register 0x00\n";
            healthy_ = false;
            return -1.0f;
        }

        // Read register 0x01: RESULT_LSB[15:8] + COUNTER[7:4] + CRC[3:0]
        uint16_t reg1 = 0;
        if (!readRegister16(0x01, reg1)) {
            std::cerr << "[OPTI4001]  Failed to read register 0x01\n";
            healthy_ = false;
            return -1.0f;
        }

        // Extract fields according to datasheet
        uint8_t exponent = (reg0 >> 12) & 0x0F;           // Bits[15:12] of reg 0x00
        uint16_t result_msb = reg0 & 0x0FFF;              // Bits[11:0] of reg 0x00
        uint8_t result_lsb = (reg1 >> 8) & 0xFF;          // Bits[15:8] of reg 0x01
        uint8_t counter = (reg1 >> 4) & 0x0F;             // Bits[7:4] of reg 0x01
        // uint8_t crc = reg1 & 0x0F;                     // Bits[3:0] of reg 0x01 (TODO: add CRC validation)

        // Calculate 20-bit MANTISSA
        // MANTISSA = (RESULT_MSB << 8) + RESULT_LSB
        uint32_t mantissa = ((uint32_t)result_msb << 8) | result_lsb;

        // Debug output and saturation detection
        static int debug_count = 0;
        bool mantissa_saturated = (mantissa >= 0xFFF00);  // Near max (20-bit = 0xFFFFF)
        bool exponent_low = (exponent <= 3);  // Stuck at low exponent

        if (debug_count++ < 10 || (mantissa_saturated && exponent_low)) {
            std::cout << "[OPTI4001]  Raw: LSB=0x" << std::hex << result_lsb
                      << " MSB=0x" << result_msb
                      << " EXP=0x" << (int)exponent
                      << " CNT=0x" << (int)counter << std::dec << "\n";
            std::cout << "[OPTI4001]  mantissa=" << mantissa << " exp=" << (int)exponent;
            if (mantissa_saturated && exponent_low) {
                std::cout << " [WARNING: Saturation! Auto-range not increasing exponent]";
            }
        }

        // Calculate ADC_CODES
        // ADC_CODES = MANTISSA * 2^E
        uint32_t adc_codes = mantissa << exponent;

        // Calculate lux
        // For PicoStar variant: lux = ADC_CODES * 312.5E-6
        // For SOT-5X3 variant: lux = ADC_CODES * 437.5E-6
        // Using SOT-5X3 formula
        float lux = (float)adc_codes * 437.5e-6f;

        if (debug_count <= 10) {
            std::cout << " lux=" << lux << "\n";
        }

        // Sanity check (max ~118klux for SOT-5X3 variant)
        // Max = (2^20 - 1) * 2^8 * 437.5e-6 = 118,362 lux
        if (lux < 0.0f || lux > 120000.0f) {
            if (debug_count <= 10 || lux > 120000.0f) {
                std::cerr << "[OPTI4001]  WARNING: lux out of range: " << lux << "\n";
            }
            // Don't fail, just clamp
            if (lux < 0.0f) lux = 0.0f;
            if (lux > 120000.0f) lux = 120000.0f;
        }

        healthy_ = true;
        return lux;
    }

    bool isHealthy() const override {
        return healthy_;
    }

    std::string getType() const override {
        return "opti4001";
    }

private:
    bool readRegister16(uint8_t reg, uint16_t& value) {
        // Write register address
        if (write(i2c_fd_, &reg, 1) != 1) {
            std::cerr << "[OPTI4001]  Failed to write register address: " << strerror(errno) << "\n";
            return false;
        }

        // Read 16-bit value (MSB first)
        uint8_t buf[2];
        if (read(i2c_fd_, buf, 2) != 2) {
            std::cerr << "[OPTI4001]  Failed to read register value: " << strerror(errno) << "\n";
            return false;
        }

        // Combine bytes (MSB first / big endian)
        value = (buf[0] << 8) | buf[1];
        return true;
    }

    bool writeRegister16(uint8_t reg, uint16_t value) {
        uint8_t buf[3];
        buf[0] = reg;
        buf[1] = (value >> 8) & 0xFF;  // MSB
        buf[2] = value & 0xFF;         // LSB

        if (write(i2c_fd_, buf, 3) != 3) {
            std::cerr << "[OPTI4001]  Failed to write register: " << strerror(errno) << "\n";
            return false;
        }

        return true;
    }

    std::string device_;
    uint8_t address_;
    int i2c_fd_;
    bool healthy_;
};

// Factory function
std::unique_ptr<SensorInterface> createOPTI4001Sensor(const std::string& device, const std::string& address_str) {
    // Parse hex address string (e.g., "0x44")
    uint8_t address = std::stoi(address_str, nullptr, 16);
    return std::make_unique<OPTI4001Sensor>(device, address);
}

} // namespace als_dimmer
