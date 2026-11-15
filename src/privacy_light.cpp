#include "als-dimmer/privacy_light.hpp"
#include "als-dimmer/logger.hpp"

#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

namespace als_dimmer {

namespace {
constexpr uint16_t REG_PRIVACY_LEVEL = 0x0202;   // + 0x0203 PRIVACY_STATUS in the same read
constexpr int LEVEL_MAX = 100;
constexpr int WRITE_TRIES = 3;
// Not found yet: every 5 s for the first 2 minutes (the display link and the
// IOC can come up after the daemon), then once a minute. A display without
// the light costs one 2-byte read a minute.
constexpr int FAST_PROBES = 24;
constexpr int FAST_PROBE_SEC = 5;
constexpr int SLOW_PROBE_SEC = 60;
}  // namespace

PrivacyLight::PrivacyLight(const PrivacyLightConfig& cfg)
    : cfg_(cfg), next_check_(std::chrono::steady_clock::now()) {}

PrivacyLight::~PrivacyLight() { closeFd(); }

void PrivacyLight::setDesired(int level) {
    desired_ = std::max(0, std::min(LEVEL_MAX, level));
}

bool PrivacyLight::ensureOpen() {
    if (fd_ >= 0) {
        return true;
    }
    fd_ = open(cfg_.i2c_device.c_str(), O_RDWR);
    return fd_ >= 0;
}

void PrivacyLight::closeFd() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

// One combined write-pointer + read transfer (I2C_RDWR on an unbound node, as
// the other 0x66 clients do, so the kernel serialises it against them).
bool PrivacyLight::readPair(int& level, int& status) {
    if (!ensureOpen()) {
        return false;
    }
    uint8_t ptr[2] = { static_cast<uint8_t>(REG_PRIVACY_LEVEL >> 8),
                       static_cast<uint8_t>(REG_PRIVACY_LEVEL & 0xFF) };
    uint8_t buf[2] = { 0xFF, 0xFF };
    struct i2c_msg msgs[2];
    msgs[0].addr = static_cast<uint16_t>(cfg_.i2c_address);
    msgs[0].flags = 0;
    msgs[0].len = sizeof(ptr);
    msgs[0].buf = ptr;
    msgs[1].addr = static_cast<uint16_t>(cfg_.i2c_address);
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = sizeof(buf);
    msgs[1].buf = buf;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = msgs;
    xfer.nmsgs = 2;
    if (ioctl(fd_, I2C_RDWR, &xfer) < 0) {
        return false;
    }
    level = buf[0];
    status = buf[1];
    return true;
}

bool PrivacyLight::writeLevel(int level) {
    if (!ensureOpen()) {
        return false;
    }
    uint8_t data[3] = { static_cast<uint8_t>(REG_PRIVACY_LEVEL >> 8),
                        static_cast<uint8_t>(REG_PRIVACY_LEVEL & 0xFF),
                        static_cast<uint8_t>(level) };
    struct i2c_msg msg;
    msg.addr = static_cast<uint16_t>(cfg_.i2c_address);
    msg.flags = 0;
    msg.len = sizeof(data);
    msg.buf = data;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = &msg;
    xfer.nmsgs = 1;
    return ioctl(fd_, I2C_RDWR, &xfer) >= 0;
}

bool PrivacyLight::apply(int level) {
    setDesired(level);
    if (!available_) {
        return false;
    }
    // Write, then read back: a write the 983/988 path drops must not pass as done.
    for (int i = 0; i < WRITE_TRIES; ++i) {
        int lv = -1;
        int st = -1;
        if (writeLevel(desired_) && readPair(lv, st) && lv == desired_) {
            reachable_ = true;
            mcu_level_ = lv;
            running_ = (st == 0x01);
            return true;
        }
    }
    reachable_ = false;
    LOG_WARN("privacy", "Level " << desired_ << " % did not take; retrying at the next check");
    return false;
}

void PrivacyLight::poll() {
    if (!cfg_.enabled) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_check_) {
        return;
    }

    int lv = -1;
    int st = -1;
    const bool answered = readPair(lv, st);
    // 0x0203 is 0x00 / 0x01 only where the light exists; 0xFF (reserved) or
    // no answer means "not here".
    const bool supported = answered && lv <= LEVEL_MAX && (st == 0x00 || st == 0x01);

    if (!available_) {
        if (supported) {
            available_ = true;
            LOG_INFO("privacy", "Privacy light found on " << cfg_.i2c_device << " 0x" << std::hex
                     << cfg_.i2c_address << std::dec << " (MCU level " << lv << " %, "
                     << (st == 0x01 ? "running" : "stopped") << "); holding " << desired_ << " %");
        } else {
            ++probe_count_;
            if (!answered) {
                closeFd();   // the node may not exist yet; reopen at the next probe
            }
            next_check_ = now + std::chrono::seconds(probe_count_ < FAST_PROBES ? FAST_PROBE_SEC
                                                                                : SLOW_PROBE_SEC);
            return;
        }
    }

    next_check_ = now + std::chrono::seconds(std::max(1, cfg_.poll_interval_sec));
    if (!supported) {
        // Found before, not answering now (display link down, IOC resetting): keep
        // the slider, retry at the next check.
        reachable_ = false;
        return;
    }
    reachable_ = true;
    running_ = (st == 0x01);
    mcu_level_ = lv;
    if (lv != desired_) {
        LOG_INFO("privacy", "MCU holds " << lv << " %, restoring " << desired_ << " %");
        apply(desired_);
    }
}

nlohmann::json PrivacyLight::status() const {
    return nlohmann::json{
        {"available", available_},
        {"level", desired_},
        {"running", running_},
        {"reachable", reachable_},
        {"mcu_level", mcu_level_ >= 0 ? nlohmann::json(mcu_level_) : nlohmann::json(nullptr)},
    };
}

}  // namespace als_dimmer
