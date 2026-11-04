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
 * Backlight dimming of a 3x QVue display board running rh850-baremetal
 * display_manager (QVUE_9090, v00.23+ / every release v01.00+), through its
 * IOC slave at 0x66 behind the DS90UH983/988 pass-through. The stock firmware
 * (and qvue_legacy) speak the vendor protocol at 0x32 instead: output type
 * "qvue". Which one a unit runs is decided by als-dimmer-select-config.sh.
 *
 * Registers (rh850-baremetal docs/i2c_register_map.md, "FPGA brightness
 * block"), all big-endian, one block per FPGA output:
 *   0x0240 + 4i  OUT_REQ[i] hi   RW, staged
 *   0x0241 + 4i  OUT_REQ[i] lo   RW, writing it commits the request (clamped to MAX)
 *   0x0214       BRIGHT_MAX      RO, 0x07FF
 * Output i drives display i+1. 0x0210 (BRIGHT_REQ, what ioc_tcon writes) is
 * OUT_REQ[0] only, which is why this is a separate output type.
 *
 * Scale: the FPGA's 12-bit level 1:1, MAX 0x07FF. It is half the stock host
 * scale (the firmware's stock-equivalent mapping), so value_range [20, 2047]
 * with response_exponent 2.2 gives the same percent -> nits curve as the
 * "qvue" config's [40, 4095]: the backlight lights from about 12-16 counts
 * (rig, i1 Display Pro, 2026-11-04: 12 -> 1.8, 16 -> 14, 20 -> 35, 30 -> 83,
 * 40 -> 115, 0x100 -> 868 nits full white). off_at_zero sends 0 at 0 %.
 *
 * The 983/988 pass-through drops the next write to 0x66 after a read whose
 * data contains 0x67 (not as its first byte). Every write here is therefore
 * preceded by a plain 1-byte read (one byte cannot carry a 0x67 after its
 * first byte), the same guard disptool uses, and each request is read back
 * (hi <= 0x07, so the 2-byte read-back cannot be corrupted) and rewritten up
 * to kWriteTries times.
 *
 * Writes only on change; every kRecheckEvery calls output 0's request is read
 * and the cache dropped if it differs, so a firmware restart (boot level
 * 0x1C2, a warm OTA) is put right within seconds.
 */
class QvueIocOutput : public OutputInterface {
public:
    QvueIocOutput(const std::string& device, uint8_t address, const std::vector<int>& displays,
                  int range_lo, int range_hi, double response_exponent, bool off_at_zero)
        : device_(device), address_(address), displays_(displays),
          range_lo_(range_lo), range_hi_(range_hi), exponent_(response_exponent),
          off_at_zero_(off_at_zero), i2c_fd_(-1), cached_percent_(-1), cached_raw_(-1),
          write_count_(0), fail_count_(0), calls_(0) {}

    ~QvueIocOutput() override {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[QVUE_IOC] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec
                  << " (display_manager OUT_REQ 0x0240+4i), displays";
        for (int d : displays_) {
            std::cout << " " << d;
        }
        std::cout << ", raw range [" << range_lo_ << ", " << range_hi_
                  << "], response exponent " << exponent_
                  << (off_at_zero_ ? ", 0 % = off" : "") << std::endl;

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE_IOC] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // Not fatal: the link may come up after the daemon (the hh983 driver
        // retries its probe). The first setBrightness() writes regardless.
        guard();
        const int fw_max = readU16(kRegBrightMax);
        if (fw_max < 0) {
            std::cerr << "[QVUE_IOC] BRIGHT_MAX (0x0214) not readable yet; writing anyway\n";
        } else {
            std::cout << "[QVUE_IOC] firmware MAX " << fw_max << std::endl;
            if (fw_max != 0xFFFF && range_hi_ > fw_max) {
                std::cerr << "[QVUE_IOC] config value_range max " << range_hi_
                          << " is above the firmware's MAX " << fw_max << "; it will clamp\n";
            }
        }
        return true;
    }

    bool setBrightness(int brightness) override {
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE_IOC] Output not initialized\n";
            return false;
        }
        int pct = brightness;
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }

        if (pct == cached_percent_ && (++calls_ % kRecheckEvery) == 0) {
            guard();
            const int req0 = readU16(outReg(displays_.front() - 1));
            if (req0 >= 0 && req0 != cached_raw_) {
                std::cout << "[QVUE_IOC] output " << displays_.front() - 1 << " reads " << req0
                          << ", expected " << cached_raw_ << " (firmware restarted?); rewriting\n";
                cached_percent_ = -1;
            }
        }
        if (pct == cached_percent_) {
            return true;
        }

        const int raw = percentToRaw(pct);
        for (int disp : displays_) {
            if (!writeOutput(disp - 1, raw)) {
                if (fail_count_ < 5 || (fail_count_ % 120) == 0) {
                    std::cerr << "[QVUE_IOC] Failed to set brightness " << pct << "% (raw " << raw
                              << ") on display " << disp << ": " << strerror(errno) << "\n";
                }
                fail_count_++;
                return false;   // cache untouched: the next cycle retries
            }
        }
        if (fail_count_ > 0) {
            std::cout << "[QVUE_IOC] writes succeed again after " << fail_count_ << " failures" << std::endl;
            fail_count_ = 0;
        }
        cached_percent_ = pct;
        cached_raw_ = raw;
        write_count_++;
        if (write_count_ <= 10) {
            std::cout << "[QVUE_IOC] " << pct << "% -> raw " << raw << std::endl;
        }
        return true;
    }

    int getCurrentBrightness() override {
        return cached_percent_;
    }

    std::string getType() const override {
        return "qvue_ioc";
    }

private:
    static const uint16_t kRegBrightMax = 0x0214;
    static const uint16_t kRegOutReq0   = 0x0240;
    static const int kWriteTries        = 3;
    static const unsigned kRecheckEvery = 20;   // x update interval (500 ms) = 10 s

    static uint16_t outReg(int out) { return static_cast<uint16_t>(kRegOutReq0 + 4 * out); }

    int percentToRaw(int pct) const {
        if (pct == 0 && off_at_zero_) {
            return 0;
        }
        const double frac = std::pow(pct / 100.0, exponent_);
        return range_lo_ + static_cast<int>(std::lround(frac * (range_hi_ - range_lo_)));
    }

    bool writeOutput(int out, int raw) {
        const uint16_t reg = outReg(out);
        const uint8_t frame[4] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
                                   (uint8_t)((raw >> 8) & 0xFF), (uint8_t)(raw & 0xFF) };
        for (int t = 0; t < kWriteTries; t++) {
            guard();
            if (!writeRaw(frame, sizeof(frame))) {
                continue;
            }
            guard();
            if (readU16(reg) == raw) {
                return true;
            }
        }
        return false;
    }

    /** The 983/988 write guard: a plain 1-byte read (result ignored). */
    void guard() {
        uint8_t b = 0;
        struct i2c_msg msg;
        msg.addr = address_;
        msg.flags = I2C_M_RD;
        msg.len = 1;
        msg.buf = &b;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = &msg;
        xfer.nmsgs = 1;
        (void)ioctl(i2c_fd_, I2C_RDWR, &xfer);
    }

    /** One I2C_RDWR write on an unbound device node (the hh983 driver holds 0x18). */
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

    int readU16(uint16_t reg) {
        uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
        uint8_t b[2] = { 0, 0 };
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(ptr);
        msgs[0].buf = ptr;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = 2;
        msgs[1].buf = b;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;
        if (ioctl(i2c_fd_, I2C_RDWR, &xfer) < 0) {
            return -1;
        }
        return (b[0] << 8) | b[1];
    }

    std::string device_;
    uint8_t address_;
    std::vector<int> displays_;
    int range_lo_;
    int range_hi_;
    double exponent_;
    bool off_at_zero_;
    int i2c_fd_;
    int cached_percent_;   // -1 = unknown, so the next update always writes
    int cached_raw_;
    unsigned write_count_;
    unsigned fail_count_;
    unsigned calls_;
};

std::unique_ptr<OutputInterface> createQvueIocOutput(const std::string& device,
                                                     const std::string& address_str,
                                                     const std::vector<int>& displays,
                                                     int range_lo, int range_hi,
                                                     double response_exponent,
                                                     bool off_at_zero) {
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::unique_ptr<OutputInterface>(
        new QvueIocOutput(device, address, displays, range_lo, range_hi, response_exponent,
                          off_at_zero));
}

} // namespace als_dimmer
