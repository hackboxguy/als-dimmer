#include "als-dimmer/interfaces.hpp"
#include <iostream>
#include <memory>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {

/**
 * Panel TCON brightness through the ioc-deserializer (IOC) at 0x66.
 *
 * On the OTS-OLED 17.3" driver board the panel's TCON is at 0x61 on the MCU's
 * private I2C bus and the head unit cannot reach it. The MCU exposes a typed
 * relay instead: write a raw brightness value to 0x0210 and it forwards it to
 * the TCON. See docs/i2c_register_map.md in rh850-baremetal-demo (page 0x02,
 * "Brightness relay") and docs/als-management-plan.md buckets 2, 4 and 5.
 *
 * A separate class rather than a fourth DimmerType in I2CDimmerOutput: the
 * wire format differs (a 16-bit register address, no 00 00 00 cmd header) and
 * the range has a non-zero floor -- 13 is the TCON's documented 5-nit minimum,
 * not "off".
 *
 * Register block, all big-endian:
 *   0x0210-0x0211  BRIGHT_REQ     RW, raw TCON units; writing the LOW byte commits
 *   0x0212         BRIGHT_STATUS  RO, 0x00 none / 0x01 pending / 0x02 applied / 0xFF failed
 *   0x0213         BRIGHT_MODE    RW, reserved, always 0x00 here
 *   0x0214-0x0215  BRIGHT_MAX     RO
 *   0x0216-0x0217  BRIGHT_ACTUAL  RO, 0xFFFF when the TCON offers no readback
 *   0x0218         BRIGHT_ERR     RO
 *   0x0219-0x021A  BRIGHT_MIN     RO
 *
 * The IOC applies the value asynchronously, within about 20 ms and always
 * within 200 ms, so a successful transaction here means "accepted", not
 * "on the glass". At a 500 ms update interval that distinction never matters
 * to the control loop; disptool polls BRIGHT_STATUS when a human wants to know.
 */
class IocTconOutput : public OutputInterface {
public:
    IocTconOutput(const std::string& device, uint8_t address, int range_lo, int range_hi)
        : device_(device), address_(address), range_lo_(range_lo), range_hi_(range_hi),
          i2c_fd_(-1), cached_percent_(-1), write_count_(0) {}

    ~IocTconOutput() override {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[IOC_TCON] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec
                  << " (brightness relay 0x0210)\n";

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[IOC_TCON] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // The firmware publishes the TCON's own range. Reading it back is the
        // version check: firmware without the relay leaves the whole block
        // reserved, so both halves read 0xFFFF.
        int fw_min = readU16(kRegBrightMin);
        int fw_max = readU16(kRegBrightMax);
        if (fw_min < 0 || fw_max < 0) {
            std::cerr << "[IOC_TCON] Failed to read the brightness range: "
                      << strerror(errno) << "\n";
            closeFd();
            return false;
        }
        if (fw_min == 0xFFFF || fw_max == 0xFFFF) {
            std::cerr << "[IOC_TCON] BRIGHT_MIN/BRIGHT_MAX read 0xFFFF: this IOC firmware "
                         "has no brightness relay (needs v01.17 or later)\n";
            closeFd();
            return false;
        }

        std::cout << "[IOC_TCON] range " << fw_min << "-" << fw_max
                  << " reported by the IOC, config value_range ["
                  << range_lo_ << ", " << range_hi_ << "]\n";

        // Refuse rather than clamp. The firmware rejects out-of-range requests
        // too, so clamping here would only hide a mis-scaled config until the
        // first time the algorithm reached the end of its travel.
        if (range_lo_ < fw_min || range_hi_ > fw_max) {
            std::cerr << "[IOC_TCON] config value_range [" << range_lo_ << ", " << range_hi_
                      << "] lies outside the range the IOC will forward ["
                      << fw_min << ", " << fw_max << "]; refusing to start\n";
            closeFd();
            return false;
        }

        // Seed the cache only from a value we can actually trust. The TCON has
        // no readback on this panel, so BRIGHT_ACTUAL is always 0xFFFF and the
        // cache stays "unknown", which makes the first setBrightness() write
        // unconditionally -- the right thing after a restart.
        int status = readU8(kRegBrightStatus);
        if (status == kStatusApplied) {
            int actual = readU16(kRegBrightActual);
            if (actual >= 0 && actual != 0xFFFF) {
                cached_percent_ = nativeToPercent(actual);
                std::cout << "[IOC_TCON] IOC reports " << actual
                          << " already applied (" << cached_percent_ << "%)\n";
            }
        }
        if (cached_percent_ < 0) {
            std::cout << "[IOC_TCON] no readback available; first update will write unconditionally\n";
        }

        return true;
    }

    bool setBrightness(int brightness) override {
        if (i2c_fd_ < 0) {
            std::cerr << "[IOC_TCON] Output not initialized\n";
            return false;
        }

        int pct = brightness;
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }

        if (pct == cached_percent_) {
            return true;
        }

        const int native = percentToNative(pct);
        uint8_t msg[4] = {
            (uint8_t)(kRegBrightReq >> 8), (uint8_t)(kRegBrightReq & 0xFF),
            (uint8_t)((native >> 8) & 0xFF), (uint8_t)(native & 0xFF)
        };

        // Both bytes in one auto-increment transaction: the IOC commits on the
        // low byte, so splitting them would hand the TCON half a value.
        if (!writeRaw(msg, sizeof(msg))) {
            std::cerr << "[IOC_TCON] Failed to write brightness " << pct << "% (raw "
                      << native << "): " << strerror(errno) << "\n";
            reportRelayError();
            return false;
        }

        cached_percent_ = pct;
        write_count_++;

        // The relay applies asynchronously, so a healthy transaction is not
        // proof the TCON took it. Sample the outcome now and then rather than
        // adding a read to every 500 ms cycle.
        if ((write_count_ % kStatusPollEvery) == 0) {
            reportRelayError();
        }

        if (write_count_ <= 10) {
            std::cout << "[IOC_TCON] " << pct << "% -> raw " << native << "\n";
        }
        return true;
    }

    int getCurrentBrightness() override {
        // The cached percent, not a fresh read: BRIGHT_ACTUAL is for disptool
        // and for a human, and on this panel it is 0xFFFF anyway. Same policy
        // as I2CDimmerOutput.
        return cached_percent_;
    }

    std::string getType() const override {
        return "ioc_tcon";
    }

private:
    static const uint16_t kRegBrightReq    = 0x0210;
    static const uint16_t kRegBrightStatus = 0x0212;
    static const uint16_t kRegBrightMax    = 0x0214;
    static const uint16_t kRegBrightActual = 0x0216;
    static const uint16_t kRegBrightErr    = 0x0218;
    static const uint16_t kRegBrightMin    = 0x0219;

    static const int kStatusApplied    = 0x02;
    static const int kStatusPollEvery  = 20;

    void closeFd() {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
            i2c_fd_ = -1;
        }
    }

    int percentToNative(int pct) const {
        const double span = static_cast<double>(range_hi_ - range_lo_);
        return range_lo_ + static_cast<int>(std::lround(pct * span / 100.0));
    }

    int nativeToPercent(int native) const {
        const int span = range_hi_ - range_lo_;
        if (span <= 0) {
            return 0;
        }
        int pct = static_cast<int>(std::lround((native - range_lo_) * 100.0 / span));
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }
        return pct;
    }

    /**
     * One I2C_RDWR write on an unbound device node, so this coexists with the
     * kernel hh983-serializer driver holding 0x18 without forcing anything.
     */
    bool writeRaw(const uint8_t* data, size_t len) {
        struct i2c_msg msg;
        msg.addr = address_;
        msg.flags = 0;
        msg.len = static_cast<uint16_t>(len);
        msg.buf = const_cast<uint8_t*>(data);

        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = &msg;
        xfer.nmsgs = 1;
        return ioctl(i2c_fd_, I2C_RDWR, &xfer) >= 0;
    }

    /** Read `len` bytes from a 16-bit IOC register. Returns false on any failure. */
    bool readRaw(uint16_t reg, uint8_t* out, size_t len) {
        uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(ptr);
        msgs[0].buf = ptr;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = static_cast<uint16_t>(len);
        msgs[1].buf = out;

        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;
        return ioctl(i2c_fd_, I2C_RDWR, &xfer) >= 0;
    }

    int readU8(uint16_t reg) {
        uint8_t b = 0;
        if (!readRaw(reg, &b, 1)) {
            return -1;
        }
        return b;
    }

    int readU16(uint16_t reg) {
        uint8_t b[2] = { 0, 0 };
        if (!readRaw(reg, b, 2)) {
            return -1;
        }
        return (b[0] << 8) | b[1];
    }

    static const char* errText(int err) {
        switch (err) {
        case 0x00: return "none";
        case 0x01: return "private bus not available on the IOC";
        case 0x02: return "panel domain off";
        case 0x03: return "the TCON did not ACK";
        case 0x04: return "value outside the range the IOC forwards";
        case 0x05: return "readback mismatch";
        default:   return "unknown";
        }
    }

    /** Read back what the relay made of the last request and say so if it went wrong. */
    void reportRelayError() {
        int status = readU8(kRegBrightStatus);
        int err = readU8(kRegBrightErr);
        if (status < 0 || err < 0) {
            return;
        }
        if (err != 0x00 || status == 0xFF) {
            std::cerr << "[IOC_TCON] relay reports status 0x" << std::hex << status
                      << " err 0x" << err << std::dec << " (" << errText(err) << ")\n";
        }
    }

    std::string device_;
    uint8_t address_;
    int range_lo_;
    int range_hi_;
    int i2c_fd_;
    int cached_percent_;   // -1 = unknown, so the next update always writes
    unsigned write_count_;
};

// Factory function
std::unique_ptr<OutputInterface> createIocTconOutput(const std::string& device,
                                                     const std::string& address_str,
                                                     int range_lo, int range_hi) {
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::unique_ptr<OutputInterface>(
        new IocTconOutput(device, address, range_lo, range_hi));
}

} // namespace als_dimmer
