#include "als-dimmer/privacy_mode.hpp"
#include "als-dimmer/logger.hpp"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>

namespace als_dimmer {

namespace {
constexpr uint16_t ADDR_NEW = 0x1E;
constexpr uint16_t ADDR_LEGACY = 0x1D;
constexpr uint8_t REG_VERSION = 0x00;          // 4 bytes MM DD TT VV
constexpr uint8_t REG_CHIP_ID = 0x10;          // 4 bytes 00 RES BRD|SZh SZl
constexpr uint8_t REG_BITSTREAM_TYPE = 0x1A;   // 0x5A OTA / 0xA5 GOLDEN / 0x00 legacy
constexpr uint8_t REG_PRIVACY = 0x34;          // bit 0: 1 = on
constexpr uint8_t BITSTREAM_OTA = 0x5A;
constexpr uint8_t BITSTREAM_GOLDEN = 0xA5;
constexpr int WRITE_TRIES = 3;
// Nothing answers yet: every 5 s for the first 2 minutes (the display can come
// up after the daemon), then once a minute.
constexpr int FAST_PROBES = 24;
constexpr int FAST_PROBE_SEC = 5;
constexpr int SLOW_PROBE_SEC = 60;

std::string hex8(const uint8_t* b, int n) {
    std::string s;
    char t[3];
    for (int i = 0; i < n; ++i) {
        std::snprintf(t, sizeof t, "%02X", b[i]);
        s += t;
    }
    return s;
}

std::string hexByte(int v) {
    char t[8];
    std::snprintf(t, sizeof t, "0x%02X", v & 0xFF);
    return t;
}
}  // namespace

PrivacyMode::PrivacyMode(const PrivacyModeConfig& cfg)
    : cfg_(cfg), next_check_(std::chrono::steady_clock::now()) {}

PrivacyMode::~PrivacyMode() { closeFd(); }

bool PrivacyMode::ensureOpen() {
    if (fd_ >= 0) {
        return true;
    }
    fd_ = open(cfg_.i2c_device.c_str(), O_RDWR);
    return fd_ >= 0;
}

void PrivacyMode::closeFd() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

// [page 0, reg] + read in one repeated-start transfer, so no other client can
// move the 0x1E page pointer in between.
bool PrivacyMode::readNew(uint8_t reg, uint8_t* buf, uint16_t len) {
    if (!ensureOpen()) {
        return false;
    }
    uint8_t ptr[2] = { 0x00, reg };
    struct i2c_msg msgs[2];
    msgs[0].addr = ADDR_NEW;
    msgs[0].flags = 0;
    msgs[0].len = sizeof(ptr);
    msgs[0].buf = ptr;
    msgs[1].addr = ADDR_NEW;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = buf;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = msgs;
    xfer.nmsgs = 2;
    return ioctl(fd_, I2C_RDWR, &xfer) == 2;
}

bool PrivacyMode::readLegacy(uint8_t reg, uint8_t* buf, uint16_t len) {
    if (!ensureOpen()) {
        return false;
    }
    uint8_t ptr[4] = { 0x00, 0x00, 0x00, reg };
    struct i2c_msg msgs[2];
    msgs[0].addr = ADDR_LEGACY;
    msgs[0].flags = 0;
    msgs[0].len = sizeof(ptr);
    msgs[0].buf = ptr;
    msgs[1].addr = ADDR_LEGACY;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = buf;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = msgs;
    xfer.nmsgs = 2;
    return ioctl(fd_, I2C_RDWR, &xfer) == 2;
}

bool PrivacyMode::readPrivacy(uint8_t& raw) {
    if (slave_ == Slave::New) {
        return readNew(REG_PRIVACY, &raw, 1);
    }
    if (slave_ == Slave::Legacy) {
        return readLegacy(REG_PRIVACY, &raw, 1);
    }
    return false;
}

bool PrivacyMode::writeReg(uint8_t value) {
    if (!ensureOpen() || slave_ == Slave::None) {
        return false;
    }
    uint8_t buf[5];
    struct i2c_msg msg;
    if (slave_ == Slave::New) {
        buf[0] = 0x00;            // page 0
        buf[1] = REG_PRIVACY;
        buf[2] = value;
        msg.addr = ADDR_NEW;
        msg.len = 3;
    } else {
        buf[0] = 0x00;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = REG_PRIVACY;
        buf[4] = value;
        msg.addr = ADDR_LEGACY;
        msg.len = 5;
    }
    msg.flags = 0;
    msg.buf = buf;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = &msg;
    xfer.nmsgs = 1;
    return ioctl(fd_, I2C_RDWR, &xfer) == 1;
}

void PrivacyMode::identify() {
    // Read the identity twice when it differs from what we hold: a single
    // corrupted byte must not hide (or offer) the switch.
    std::array<uint8_t, 9> a{};
    std::array<uint8_t, 9> b{};
    auto readIdent = [this](std::array<uint8_t, 9>& id) {
        return readNew(REG_VERSION, id.data(), 4) && readNew(REG_CHIP_ID, id.data() + 4, 4) &&
               readNew(REG_BITSTREAM_TYPE, id.data() + 8, 1);
    };
    if (!readIdent(a)) {
        return;
    }
    if (identified_ && a == ident_) {
        return;
    }
    if (!readIdent(b) || a != b) {
        return;   // try again at the next poll
    }
    ident_ = a;
    identified_ = true;

    const std::string chip = hex8(ident_.data() + 4, 4);
    const int version = ident_[3];
    const uint8_t type = ident_[8];
    const PrivacyModeImage* row = nullptr;
    for (const auto& img : cfg_.supported) {
        if (img.chip_id == chip) {
            row = &img;
            break;
        }
    }

    bool ok = false;
    if (!row) {
        reason_ = "FPGA chip ID " + chip + " is not in privacy_mode.supported";
    } else if (version < row->min_version) {
        reason_ = "FPGA image " + hexByte(version) + " on " + row->name + " predates the privacy drive (" +
                  hexByte(row->min_version) + "+)";
    } else if (cfg_.require_ota_slot && type != BITSTREAM_OTA) {
        reason_ = std::string("FPGA runs its ") + (type == BITSTREAM_GOLDEN ? "GOLDEN" : "unknown") +
                  " image (0x1A = " + hexByte(type) + "), which has no privacy drive";
    } else {
        ok = true;
        reason_ = row->name + " image " + hexByte(version) + " drives the privacy film";
    }

    LOG_INFO("privacy_mode", "FPGA on 0x1E: chip ID " << chip << ", image " << hexByte(version)
             << ", slot " << hexByte(type) << " -> " << (ok ? "available" : "not available")
             << " (" << reason_ << ")");
    available_ = ok;
}

bool PrivacyMode::writeVerified(bool on) {
    const uint8_t want = on ? 0x01 : 0x00;
    for (int i = 0; i < WRITE_TRIES; ++i) {
        uint8_t raw = 0xFF;
        if (writeReg(want) && readPrivacy(raw) && (raw & 0x01) == want) {
            reachable_ = true;
            fpga_on_ = raw & 0x01;
            return true;
        }
    }
    LOG_WARN("privacy_mode", "Privacy " << (on ? "ON" : "OFF")
             << " did not take; retrying at the next check");
    return false;
}

bool PrivacyMode::apply(bool on) {
    desired_ = on;
    if (!available_) {
        return false;
    }
    return writeVerified(on);
}

void PrivacyMode::poll() {
    if (!cfg_.enabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_check_) {
        return;
    }

    // 0x1E first: the pixelpipe images that can drive the film all have it.
    uint8_t raw = 0xFF;
    Slave found = Slave::None;
    if (readNew(REG_PRIVACY, &raw, 1)) {
        found = Slave::New;
    } else if (readLegacy(REG_PRIVACY, &raw, 1)) {
        found = Slave::Legacy;
    }

    if (found == Slave::None) {
        reachable_ = false;
        closeFd();   // the node may come and go with the display; reopen next time
        if (slave_ == Slave::None) {
            ++probe_count_;
            next_check_ = now + std::chrono::seconds(probe_count_ < FAST_PROBES ? FAST_PROBE_SEC
                                                                                : SLOW_PROBE_SEC);
            reason_ = "no FPGA answers on 0x1E or 0x1D";
        } else {
            // Found before (FPGA restarting, display link down): keep what we know,
            // check again soon. The restarted image is identified afresh.
            next_check_ = now + std::chrono::seconds(std::max(1, cfg_.poll_interval_sec));
        }
        return;
    }

    if (found != slave_) {
        LOG_INFO("privacy_mode", "FPGA answers on " << (found == Slave::New ? "0x1E" : "0x1D only")
                 << "; privacy register 0x34 = " << hexByte(raw));
        slave_ = found;
        identified_ = false;
    }
    reachable_ = true;
    fpga_on_ = raw & 0x01;
    next_check_ = now + std::chrono::seconds(std::max(1, cfg_.poll_interval_sec));

    if (slave_ == Slave::Legacy) {
        // Logged once above, when the slave changed. Never written: no 0x1D-only
        // image drives the film.
        available_ = false;
        identified_ = false;
        reason_ = "FPGA answers on 0x1D only (vendor or pre-0x1E image): no privacy drive";
        next_check_ = now + std::chrono::seconds(SLOW_PROBE_SEC);
        return;
    }

    identify();
    if (!available_) {
        return;
    }
    if (fpga_on_ != (desired_ ? 1 : 0)) {
        LOG_INFO("privacy_mode", "FPGA holds privacy " << (fpga_on_ ? "ON" : "OFF") << ", restoring "
                 << (desired_ ? "ON" : "OFF"));
        writeVerified(desired_);
    }
}

nlohmann::json PrivacyMode::status() const {
    nlohmann::json j{
        {"available", available_},
        {"enabled", desired_},
        {"fpga_on", fpga_on_ >= 0 ? nlohmann::json(fpga_on_ == 1) : nlohmann::json(nullptr)},
        {"reachable", reachable_},
        {"slave", slave_ == Slave::New ? nlohmann::json("0x1E")
                  : slave_ == Slave::Legacy ? nlohmann::json("0x1D") : nlohmann::json(nullptr)},
        {"reason", reason_},
    };
    if (identified_) {
        j["chip_id"] = hex8(ident_.data() + 4, 4);
        j["version"] = hexByte(ident_[3]);
        j["slot"] = ident_[8] == BITSTREAM_OTA ? "OTA" : ident_[8] == BITSTREAM_GOLDEN ? "GOLDEN" : "unknown";
    }
    return j;
}

}  // namespace als_dimmer
