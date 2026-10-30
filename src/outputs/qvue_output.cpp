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
 * Backlight dimming of a 3x QVue projection display through its RH850 F1KM
 * "Qvue_IOC" at 0x32, behind the DS90UH983/988 pass-through.
 *
 * One Set Dimming frame per display, on the wire:
 *
 *   0x02  dummy -- the MCU's slave ISR (IIC_Isr_Stubs.c) drops the first data
 *         byte after the address. Without it the length byte is lost and the
 *         MCU's frame parser stays out of step until a power cycle.
 *   0x06  frame length (counts itself)
 *   0x03  LED backlight device
 *   0x01  write
 *   disp  1 = main, 2..4 = sub 1..3
 *   hi lo 12-bit dimming value (0x000-0xFFF), big-endian
 *
 * Write-only, deliberately. Every read from 0x32 tried on the bench (a plain
 * read, or a repeated-START read after a valid Get Dimming frame) returned
 * zeros and took the MCU and the 988 off the bus until a power cycle, and an
 * i2cdetect-style probe did the same. So init() opens the bus and touches
 * nothing, and getCurrentBrightness() reports what was last sent.
 *
 * Writes only on change. The daemon calls setBrightness() every update cycle
 * in MANUAL mode, and the MCU stores every Set Dimming in its NV memory, so
 * resending an unchanged level would wear its flash at two writes a second.
 *
 * Frames to different displays are spaced kFrameGapUs apart. The MCU hands
 * every frame to its FPGA manager through ONE shared (value, display) pair,
 * and the manager runs later, in a lower-priority task that busy-waits 100 ms
 * on each main-display change (CtAp_FPGA_Manager.c, delay(100)). A frame for
 * display 2 that lands before the manager has taken display 1's overwrites
 * it: sent back to back, display 1 kept its old level on the bench
 * (2026-10-30). 150 ms clears the busy-wait with margin.
 *
 * The MCU's response is linear in the raw value (about 1.6 nits per count on
 * full white, measured 2026-10-30), which makes linear percent steps far too
 * coarse at the dark end of a ~6,000-nit panel; response_exponent bends the
 * percent scale so the brightness buttons step evenly to the eye.
 *
 * off_at_zero makes 0 % switch the backlight off (raw 0) while 1-100 % keep
 * the range above: lowering the floor instead would black out the whole
 * dark end of the curve, since raw values below ~33 do not light it.
 */
class QvueOutput : public OutputInterface {
public:
    QvueOutput(const std::string& device, uint8_t address, const std::vector<int>& displays,
               int range_lo, int range_hi, double response_exponent, bool off_at_zero)
        : device_(device), address_(address), displays_(displays),
          range_lo_(range_lo), range_hi_(range_hi), exponent_(response_exponent),
          off_at_zero_(off_at_zero),
          i2c_fd_(-1), cached_percent_(-1), write_count_(0), fail_count_(0) {}

    ~QvueOutput() override {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[QVUE] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec
                  << ", displays";
        for (int d : displays_) {
            std::cout << " " << d;
        }
        std::cout << ", raw range [" << range_lo_ << ", " << range_hi_
                  << "], response exponent " << exponent_
                  << (off_at_zero_ ? ", 0 % = off" : "") << std::endl;

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // No probe: the MCU cannot be read, and the cache stays "unknown" so
        // the first setBrightness() writes unconditionally -- which also puts
        // the MCU back in step with the daemon after a restart.
        std::cout << "[QVUE] write-only output; first update will write unconditionally" << std::endl;
        return true;
    }

    bool setBrightness(int brightness) override {
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE] Output not initialized\n";
            return false;
        }

        int pct = brightness;
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }

        if (pct == cached_percent_) {
            return true;
        }

        const int raw = percentToRaw(pct);
        bool first = true;
        for (int disp : displays_) {
            if (!first) {
                usleep(kFrameGapUs);
            }
            first = false;
            const uint8_t frame[7] = {
                0x02, 0x06, 0x03, 0x01,
                static_cast<uint8_t>(disp),
                static_cast<uint8_t>((raw >> 8) & 0x0F),
                static_cast<uint8_t>(raw & 0xFF)
            };
            if (!writeRaw(frame, sizeof(frame))) {
                // Leave the cache alone so the next cycle retries, but do not
                // fill the journal at the update rate while the link is down.
                if (fail_count_ < 5 || (fail_count_ % 120) == 0) {
                    std::cerr << "[QVUE] Failed to write brightness " << pct << "% (raw "
                              << raw << ") to display " << disp << ": " << strerror(errno)
                              << "\n";
                }
                fail_count_++;
                return false;
            }
        }

        if (fail_count_ > 0) {
            std::cout << "[QVUE] writes succeed again after " << fail_count_ << " failures" << std::endl;
            fail_count_ = 0;
        }
        cached_percent_ = pct;
        write_count_++;
        if (write_count_ <= 10) {
            std::cout << "[QVUE] " << pct << "% -> raw " << raw << std::endl;
        }
        return true;
    }

    int getCurrentBrightness() override {
        // What was last sent; the MCU cannot be read back (see above).
        return cached_percent_;
    }

    std::string getType() const override {
        return "qvue";
    }

private:
    static const useconds_t kFrameGapUs = 150000;

    int percentToRaw(int pct) const {
        if (pct == 0 && off_at_zero_) {
            return 0;
        }
        const double frac = std::pow(pct / 100.0, exponent_);
        return range_lo_ + static_cast<int>(std::lround(frac * (range_hi_ - range_lo_)));
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

    std::string device_;
    uint8_t address_;
    std::vector<int> displays_;
    int range_lo_;
    int range_hi_;
    double exponent_;
    bool off_at_zero_;
    int i2c_fd_;
    int cached_percent_;   // -1 = unknown, so the next update always writes
    unsigned write_count_;
    unsigned fail_count_;
};

// Factory function
std::unique_ptr<OutputInterface> createQvueOutput(const std::string& device,
                                                  const std::string& address_str,
                                                  const std::vector<int>& displays,
                                                  int range_lo, int range_hi,
                                                  double response_exponent,
                                                  bool off_at_zero) {
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::unique_ptr<OutputInterface>(
        new QvueOutput(device, address, displays, range_lo, range_hi, response_exponent,
                       off_at_zero));
}

} // namespace als_dimmer
