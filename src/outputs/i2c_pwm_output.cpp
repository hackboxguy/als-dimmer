#include "als-dimmer/outputs/i2c_pwm_output.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace als_dimmer {

namespace {

// MPQ3367 LED driver register init values (same as BoePwmOutput uses).
// The MPQ3367 is downstream of the i2c-tiny-usb-pwm's PB1 PWM line on
// typical BOE wiring; both chips sit on the same I2C bus the dongle
// exposes. Init is idempotent.
constexpr uint8_t MPQ3367_ADDR        = 0x38;
constexpr uint8_t MPQ3367_REG_CFG0    = 0x00;
constexpr uint8_t MPQ3367_VAL_CFG0    = 0xA6;  // OTID=1, MODE=01 pure PWM, FSPMF=11
constexpr uint8_t MPQ3367_REG_CFG1    = 0x01;
constexpr uint8_t MPQ3367_VAL_CFG1    = 0xAA;  // PSE=1, TH_S=01, FSW=01, CH=010 (4 strings)
constexpr uint8_t MPQ3367_REG_FAULT   = 0x02;  // Read twice to clear FT_LEDO latch

} // namespace

I2CPwmOutput::I2CPwmOutput(const std::string& device,
                           uint8_t address,
                           uint8_t duty_register,
                           uint8_t enable_register,
                           uint8_t enable_value,
                           bool skip_pwm_enable,
                           int max_value,
                           bool skip_chip_config)
    : device_(device)
    , address_(address)
    , duty_register_(duty_register)
    , enable_register_(enable_register)
    , enable_value_(enable_value)
    , skip_pwm_enable_(skip_pwm_enable)
    , max_value_(max_value > 0 ? max_value : 255)
    , skip_chip_config_(skip_chip_config)
    , fd_(-1)
    , current_brightness_(-1)
    , last_native_value_(-1) {}

I2CPwmOutput::~I2CPwmOutput() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

std::string I2CPwmOutput::getType() const {
    return "i2c_pwm";
}

int I2CPwmOutput::getCurrentBrightness() {
    return current_brightness_ < 0 ? 0 : current_brightness_;
}

bool I2CPwmOutput::writeI2cByte(uint8_t slave_addr, uint8_t reg, uint8_t value) {
    if (fd_ < 0) return false;
    if (ioctl(fd_, I2C_SLAVE, slave_addr) < 0) {
        LOG_ERROR("I2CPwmOutput", "I2C_SLAVE 0x"
                  << std::hex << static_cast<int>(slave_addr) << std::dec
                  << " failed: " << std::strerror(errno));
        return false;
    }
    uint8_t buf[2] = {reg, value};
    if (write(fd_, buf, 2) != 2) {
        LOG_ERROR("I2CPwmOutput", "I2C write to 0x"
                  << std::hex << static_cast<int>(slave_addr)
                  << " reg 0x" << static_cast<int>(reg) << std::dec
                  << " failed: " << std::strerror(errno));
        return false;
    }
    return true;
}

bool I2CPwmOutput::configureMpq3367() {
    if (ioctl(fd_, I2C_SLAVE, MPQ3367_ADDR) < 0) {
        LOG_ERROR("I2CPwmOutput", "MPQ3367 I2C_SLAVE failed: " << std::strerror(errno));
        return false;
    }
    if (!writeI2cByte(MPQ3367_ADDR, MPQ3367_REG_CFG0, MPQ3367_VAL_CFG0)) return false;
    if (!writeI2cByte(MPQ3367_ADDR, MPQ3367_REG_CFG1, MPQ3367_VAL_CFG1)) return false;

    // Clear the FT_LEDO power-on latch: write the register pointer, then
    // discard two reads. Failures here are tolerated - the latch clearing
    // is a best-effort detail, not load-bearing for brightness control.
    uint8_t reg = MPQ3367_REG_FAULT;
    uint8_t v = 0;
    (void)write(fd_, &reg, 1);
    (void)read(fd_, &v, 1);
    (void)read(fd_, &v, 1);

    LOG_DEBUG("I2CPwmOutput", "MPQ3367 configured: 0x00<-0xA6, 0x01<-0xAA, FT_LEDO cleared");
    return true;
}

bool I2CPwmOutput::enablePwm() {
    if (!writeI2cByte(address_, enable_register_, enable_value_)) {
        LOG_WARN("I2CPwmOutput", "PWM enable register write failed (slave 0x"
                 << std::hex << static_cast<int>(address_)
                 << " reg 0x" << static_cast<int>(enable_register_)
                 << " <- 0x" << static_cast<int>(enable_value_) << std::dec
                 << "); the dongle's hardware default is enabled+50% duty, "
                 "so brightness control may still work but state is unverified");
        return false;
    }
    LOG_DEBUG("I2CPwmOutput", "PWM enabled: slave 0x"
              << std::hex << static_cast<int>(address_)
              << " reg 0x" << static_cast<int>(enable_register_)
              << " <- 0x" << static_cast<int>(enable_value_) << std::dec);
    return true;
}

bool I2CPwmOutput::init() {
    fd_ = open(device_.c_str(), O_RDWR);
    if (fd_ < 0) {
        LOG_ERROR("I2CPwmOutput", "Failed to open " << device_
                  << ": " << std::strerror(errno));
        return false;
    }

    // Init the LED driver chip first - if it isn't configured, even
    // correct duty values don't produce light.
    if (!skip_chip_config_) {
        if (!configureMpq3367()) {
            LOG_ERROR("I2CPwmOutput", "MPQ3367 init failed");
            close(fd_);
            fd_ = -1;
            return false;
        }
    } else {
        LOG_INFO("I2CPwmOutput", "skip_chip_config=true, not touching MPQ3367");
    }

    // Then enable the PWM source. Failure is logged but non-fatal because
    // the dongle defaults to enabled at boot - the daemon can still drive
    // brightness even if this write didn't go through.
    if (!skip_pwm_enable_) {
        (void)enablePwm();
    }

    LOG_INFO("I2CPwmOutput", "Initialized: device=" << device_
             << " pwm_slave=0x" << std::hex << static_cast<int>(address_)
             << " duty_reg=0x" << static_cast<int>(duty_register_) << std::dec
             << " max_value=" << max_value_
             << " mpq3367=" << (skip_chip_config_ ? "skipped" : "configured"));
    return true;
}

bool I2CPwmOutput::setBrightness(int brightness) {
    brightness = std::max(0, std::min(100, brightness));
    int native_value = (brightness * max_value_ + 50) / 100;  // round to nearest
    if (native_value == last_native_value_) {
        current_brightness_ = brightness;
        return true;
    }
    if (native_value < 0) native_value = 0;
    if (native_value > max_value_) native_value = max_value_;

    if (!writeI2cByte(address_, duty_register_,
                      static_cast<uint8_t>(native_value))) {
        return false;
    }

    last_native_value_ = native_value;
    current_brightness_ = brightness;
    LOG_TRACE("I2CPwmOutput", "brightness=" << brightness
              << "% -> duty=" << native_value << "/" << max_value_);
    return true;
}

std::unique_ptr<OutputInterface> createI2CPwmOutput(const std::string& device,
                                                    uint8_t address,
                                                    uint8_t duty_register,
                                                    uint8_t enable_register,
                                                    uint8_t enable_value,
                                                    bool skip_pwm_enable,
                                                    int max_value,
                                                    bool skip_chip_config) {
    return std::unique_ptr<OutputInterface>(new I2CPwmOutput(
        device, address, duty_register, enable_register, enable_value,
        skip_pwm_enable, max_value, skip_chip_config));
}

} // namespace als_dimmer
