#include "als-dimmer/interfaces.hpp"
#include <iostream>
#include <memory>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {

/**
 * OPT5001 ambient light sensor read through the ioc-deserializer (IOC) at 0x66.
 *
 * On the OTS-OLED 17.3" driver board the panel's OPT5001 sits at 0x46 on the
 * MCU's *private* I2C bus, which the head unit cannot reach at all. The MCU
 * firmware samples it every 100 ms and publishes the result on its diagnostics
 * page; this class reads that block. See docs/i2c_register_map.md in
 * rh850-baremetal-demo (page 0x10, "ALS sample block") and
 * docs/als-management-plan.md buckets 2, 3 and 5.
 *
 * Wire format -- one 12-byte block starting at register 0x1010, big-endian:
 *
 *   byte  0     ALS_STATUS   bitfield, see kStatus* below
 *   byte  1     ALS_SEQ      wraps; the same value twice means a stuck sampler
 *   bytes 2-5   ALS_CODES    uint32, mantissa << exponent -- the value we use
 *   bytes 6-7   ALS_MANTISSA raw OPT5001 register 0x00
 *   byte  8     ALS_EXP      raw exponent 0..7
 *   byte  9     ALS_AGE      time since the last sample, 10 ms units
 *   bytes 10-11 ALS_ERR_COUNT failed transactions since MCU boot
 *
 * The value is NOT ambient lux. The sensor is behind the glass of a panel that
 * emits continuously, so a reading is k * panel_luminance + ambient: black
 * about 200 codes, the launcher UI 300..400, full white about 12000, with
 * external light adding roughly 700..1000 on top. Zones for this display are
 * therefore expressed in ADC codes with scale_factor 1.0; see
 * configs/config_ioc_opt5001_tcon_ots17.json and bucket 6 of the plan.
 *
 * Every access is a single I2C_RDWR message pair on an unbound device node, so
 * this coexists with the kernel hh983-serializer driver that holds 0x18 without
 * needing i2c-tools' -f.
 */
class IocOpt5001Sensor : public SensorInterface {
public:
    IocOpt5001Sensor(const std::string& device, uint8_t address, float scale_factor)
        : device_(device), address_(address), scale_factor_(scale_factor),
          i2c_fd_(-1), healthy_(false), have_last_seq_(false), last_seq_(0),
          same_seq_count_(0), last_reason_(Reason::kNone), debug_count_(0) {}

    ~IocOpt5001Sensor() override {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[IOC_OPT5001] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec
                  << " (ALS block 0x1010)\n";

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[IOC_OPT5001] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        uint8_t block[kBlockSize];
        if (!readBlock(block)) {
            std::cerr << "[IOC_OPT5001] Failed initial read of the ALS block: "
                      << strerror(errno) << "\n";
            closeFd();
            return false;
        }

        // 0xFF everywhere is the IOC's answer for an unimplemented register:
        // firmware older than v01.16 has no sampler at all.
        if (block[0] == 0xFF && block[1] == 0xFF) {
            std::cerr << "[IOC_OPT5001] ALS block reads 0xFF: this IOC firmware has no "
                         "ALS sampler (needs v01.16 or later)\n";
            closeFd();
            return false;
        }

        if ((block[0] & kStatusPrivBus) == 0) {
            std::cerr << "[IOC_OPT5001] PRIV_BUS = 0: the IOC has no private bus on this "
                         "board, so it can never read the sensor. The ALS is on the head "
                         "unit's own bus here -- use a direct 'opti4001'-style sensor "
                         "instead of 'ioc_opt5001'.\n";
            closeFd();
            return false;
        }

        // PANEL_ON = 0 is not an init failure: the panel may simply be off. The
        // sensor then reports unhealthy until it comes back, which is what the
        // daemon's sensor_error_timeout_sec / fallback_brightness logic is for.
        std::cout << "[IOC_OPT5001] Initialized, status 0x" << std::hex
                  << (int)block[0] << std::dec << " (" << describeStatus(block[0]) << "), "
                  << "codes " << decodeCodes(block) << ", scale_factor " << scale_factor_ << "\n";
        if ((block[0] & kStatusPanelOn) == 0) {
            std::cout << "[IOC_OPT5001] Panel domain is off; readings start when it comes on\n";
        }

        healthy_ = (block[0] & kStatusValid) != 0;
        return true;
    }

    float readLux() override {
        if (i2c_fd_ < 0) {
            std::cerr << "[IOC_OPT5001] Sensor not initialized\n";
            healthy_ = false;
            return -1.0f;
        }

        uint8_t block[kBlockSize];
        if (!readBlock(block)) {
            reportOnce(Reason::kBusError, std::string("I2C transaction failed: ") + strerror(errno));
            healthy_ = false;
            return -1.0f;
        }

        const uint8_t status = block[0];
        const uint8_t seq = block[1];
        const uint32_t codes = decodeCodes(block);

        if (debug_count_ < 10) {
            debug_count_++;
            std::cout << "[IOC_OPT5001] Raw bytes:";
            for (size_t i = 0; i < kBlockSize; i++) {
                std::cout << " 0x" << std::hex << (int)block[i];
            }
            std::cout << std::dec << "\n";
            std::cout << "[IOC_OPT5001] status 0x" << std::hex << (int)status << std::dec
                      << " (" << describeStatus(status) << ") seq " << (int)seq
                      << " codes " << codes
                      << " mantissa " << (((uint32_t)block[6] << 8) | block[7])
                      << " exp " << (int)block[8]
                      << " age " << (int)block[9] * 10 << " ms"
                      << " errors " << (((uint32_t)block[10] << 8) | block[11]) << "\n";
        }

        // A sampler that has stopped publishing looks exactly like a perfectly
        // steady light level, so the sequence counter is what tells them apart.
        // Five identical reads is 2.5 s at the default 500 ms interval, against
        // a sampler that should advance the sequence ten times a second.
        if (have_last_seq_ && seq == last_seq_) {
            same_seq_count_++;
        } else {
            same_seq_count_ = 0;
        }
        last_seq_ = seq;
        have_last_seq_ = true;

        if (same_seq_count_ >= kStuckSeqReads) {
            reportOnce(Reason::kStuck, "IOC sampler is stuck: ALS_SEQ unchanged over "
                                       + std::to_string(kStuckSeqReads + 1) + " reads");
            healthy_ = false;
            return -1.0f;
        }

        if ((status & kStatusValid) == 0) {
            reportOnce(reasonFor(status), invalidMessage(status));
            healthy_ = false;
            return -1.0f;
        }

        reportOnce(Reason::kOk, "reading valid again");
        healthy_ = true;
        return static_cast<float>(codes) * scale_factor_;
    }

    bool isHealthy() const override {
        return healthy_;
    }

    std::string getType() const override {
        return "ioc_opt5001";
    }

private:
    static const size_t kBlockSize = 12;
    static const uint16_t kRegAlsBlock = 0x1010;
    static const int kStuckSeqReads = 5;

    // ALS_STATUS bits, mirroring the IOC register map.
    static const uint8_t kStatusValid      = 0x01;
    static const uint8_t kStatusPrivBus    = 0x02;
    static const uint8_t kStatusPanelOn    = 0x04;
    static const uint8_t kStatusPresent    = 0x08;
    static const uint8_t kStatusConverting = 0x10;
    static const uint8_t kStatusSaturated  = 0x20;
    static const uint8_t kStatusError      = 0x80;

    enum class Reason { kNone, kOk, kPanelOff, kAbsent, kNoSync, kBusError, kStale, kStuck };

    void closeFd() {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
            i2c_fd_ = -1;
        }
    }

    /**
     * One I2C_RDWR pair: write the 16-bit register pointer, then read the block
     * with a repeated start. The device node is deliberately left unbound (no
     * I2C_SLAVE ioctl) so the kernel's hh983-serializer driver keeps its own
     * claim on the bus and neither of us needs to force anything.
     */
    bool readBlock(uint8_t* out) {
        uint8_t ptr[2] = { (uint8_t)(kRegAlsBlock >> 8), (uint8_t)(kRegAlsBlock & 0xFF) };
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(ptr);
        msgs[0].buf = ptr;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = kBlockSize;
        msgs[1].buf = out;

        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;

        return ioctl(i2c_fd_, I2C_RDWR, &xfer) >= 0;
    }

    static uint32_t decodeCodes(const uint8_t* block) {
        return ((uint32_t)block[2] << 24) | ((uint32_t)block[3] << 16)
             | ((uint32_t)block[4] << 8)  | (uint32_t)block[5];
    }

    static Reason reasonFor(uint8_t status) {
        if ((status & kStatusPanelOn) == 0)    { return Reason::kPanelOff; }
        if ((status & kStatusError) != 0)      { return Reason::kBusError; }
        if ((status & kStatusPresent) == 0)    { return Reason::kAbsent; }
        if ((status & kStatusConverting) == 0) { return Reason::kNoSync; }
        return Reason::kStale;
    }

    static std::string invalidMessage(uint8_t status) {
        switch (reasonFor(status)) {
        case Reason::kPanelOff: return "panel off (the sensor is in the panel power domain)";
        case Reason::kBusError: return "IOC private-bus error reading the sensor";
        case Reason::kAbsent:   return "sensor absent (no ACK at 0x46)";
        case Reason::kNoSync:   return "no SYNC: the sensor is alive but no conversions run";
        default:                return "reading stale (ALS_AGE over 500 ms)";
        }
    }

    static std::string describeStatus(uint8_t s) {
        std::string out;
        const struct { uint8_t bit; const char* name; } bits[] = {
            { kStatusValid,      "VALID" },
            { kStatusPrivBus,    "PRIV_BUS" },
            { kStatusPanelOn,    "PANEL_ON" },
            { kStatusPresent,    "PRESENT" },
            { kStatusConverting, "CONVERTING" },
            { kStatusSaturated,  "SATURATED" },
            { kStatusError,      "ERROR" },
        };
        for (const auto& b : bits) {
            if ((s & b.bit) != 0) {
                if (!out.empty()) { out += "|"; }
                out += b.name;
            }
        }
        return out.empty() ? std::string("none") : out;
    }

    // Log a reason only when it changes, so a panel that stays off for an hour
    // costs one line rather than 7200.
    void reportOnce(Reason reason, const std::string& message) {
        if (reason == last_reason_) {
            return;
        }
        last_reason_ = reason;
        if (reason == Reason::kOk) {
            std::cout << "[IOC_OPT5001] " << message << "\n";
        } else {
            std::cerr << "[IOC_OPT5001] no reading: " << message << "\n";
        }
    }

    std::string device_;
    uint8_t address_;
    float scale_factor_;
    int i2c_fd_;
    bool healthy_;
    bool have_last_seq_;
    uint8_t last_seq_;
    int same_seq_count_;
    Reason last_reason_;
    int debug_count_;
};

// Factory function
std::unique_ptr<SensorInterface> createIocOpt5001Sensor(const std::string& device,
                                                        const std::string& address_str,
                                                        float scale_factor) {
    // Parse hex address string (e.g., "0x66")
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::unique_ptr<SensorInterface>(
        new IocOpt5001Sensor(device, address, scale_factor));
}

} // namespace als_dimmer
