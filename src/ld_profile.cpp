#include "als-dimmer/ld_profile.hpp"
#include "als-dimmer/logger.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {
namespace {

// pixelpipe-fpga new slave (0x1E) page-0 registers, indexed by LdRegister.
const int REG_ADDR[LD_REGISTER_COUNT]         = {0x31, 0x33, 0x43, 0x49, 0x4A};
const char* const REG_NAME[LD_REGISTER_COUNT] = {"ld_alpha", "pc_tune", "led_gain", "bl_boost", "bl_rise"};
constexpr uint8_t PAGE = 0x00;

std::string describe(const int* v) {
    std::ostringstream os;
    bool first = true;
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        if (v[k] < 0) continue;
        os << (first ? "" : " ") << REG_NAME[k] << "=" << v[k];
        first = false;
    }
    return os.str();
}

}  // namespace

LdProfileController::LdProfileController(const LdProfileConfig& cfg,
                                         const std::string& fallback_i2c_device)
    : cfg_(cfg), device_(cfg.i2c_device.empty() ? fallback_i2c_device : cfg.i2c_device) {
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) current_[k] = target_[k] = -1;
    if (!cfg_.enabled || !openBus()) {
        disabled_ = true;
        return;
    }
    // Probe: the release-ID register (0x00) must answer on the new slave.
    int v = 0;
    if (!readReg(0x00, v)) {
        LOG_WARN("ld_profile", "FPGA new slave 0x" << std::hex << cfg_.i2c_address << std::dec
                 << " does not answer on " << device_ << "; local-dimming profiles disabled");
        disabled_ = true;
        return;
    }
    LOG_INFO("ld_profile", "Local-dimming profiles enabled (" << cfg_.profiles.size()
             << " profiles, dwell " << cfg_.min_dwell_sec << " s, verify every "
             << cfg_.verify_interval_sec << " s, steps " << describe(cfg_.transition_step) << ")");
    for (const auto& p : cfg_.profiles) {
        LOG_INFO("ld_profile", "  " << p.first << ": " << describe(p.second.value));
    }
}

LdProfileController::~LdProfileController() {
    if (fd_ >= 0) close(fd_);
}

bool LdProfileController::openBus() {
    fd_ = open(device_.c_str(), O_RDWR);
    if (fd_ < 0) {
        LOG_WARN("ld_profile", "Cannot open " << device_ << ": " << strerror(errno));
        return false;
    }
    if (ioctl(fd_, I2C_SLAVE, cfg_.i2c_address) < 0) {
        LOG_WARN("ld_profile", "Cannot select I2C slave 0x" << std::hex << cfg_.i2c_address
                 << std::dec << ": " << strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool LdProfileController::writeReg(int reg, int value) {
    uint8_t buf[3] = {PAGE, static_cast<uint8_t>(reg), static_cast<uint8_t>(value)};
    return write(fd_, buf, sizeof(buf)) == static_cast<ssize_t>(sizeof(buf));
}

bool LdProfileController::readReg(int reg, int& value) {
    uint8_t ptr[2] = {PAGE, static_cast<uint8_t>(reg)};
    if (write(fd_, ptr, sizeof(ptr)) != static_cast<ssize_t>(sizeof(ptr))) return false;
    uint8_t d = 0;
    if (read(fd_, &d, 1) != 1) return false;
    value = d;
    return true;
}

// Move every register of the target profile one step (or all the way when `first`, or when
// the register's step is 0) and read it back. A register that does not hold the written value
// is not implemented by this bitstream: warn once, stop using it.
void LdProfileController::step(bool first) {
    bool settled = true;
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        if (target_[k] < 0 || (unsupported_mask_ & (1 << k))) continue;
        if (current_[k] < 0) {
            int rb = -1;                                   // start from what the FPGA holds
            if (!readReg(REG_ADDR[k], rb)) { settled = false; continue; }
            current_[k] = rb;
        }
        if (current_[k] == target_[k]) continue;
        int next = target_[k];
        const int s = cfg_.transition_step[k];
        if (!first && s > 0 && std::abs(target_[k] - current_[k]) > s) {
            next = current_[k] + (target_[k] > current_[k] ? s : -s);
        }
        int rb = -1;
        if (!writeReg(REG_ADDR[k], next) || !readReg(REG_ADDR[k], rb)) {
            settled = false;                               // bus busy / display off: next iteration
            continue;
        }
        if (rb != next) {
            unsupported_mask_ |= (1 << k);
            LOG_WARN("ld_profile", REG_NAME[k] << " (0x" << std::hex << REG_ADDR[k] << std::dec
                     << ") reads back " << rb << " after writing " << next
                     << ": not in this bitstream, dropped from the profiles");
            continue;
        }
        current_[k] = next;
        if (next != target_[k]) settled = false;
    }
    if (settled && !settled_logged_) {
        settled_logged_ = true;
        LOG_INFO("ld_profile", "Profile '" << target_zone_ << "' in place: " << describe(target_));
    }
}

void LdProfileController::verify() {
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        if (current_[k] < 0 || (unsupported_mask_ & (1 << k))) continue;
        int rb = -1;
        if (!readReg(REG_ADDR[k], rb)) return;             // display off: try again later
        if (rb != current_[k]) {
            LOG_INFO("ld_profile", REG_NAME[k] << " reads " << rb << " instead of " << current_[k]
                     << " (FPGA restarted?); re-applying '" << target_zone_ << "'");
            for (int j = 0; j < LD_REGISTER_COUNT; ++j) current_[j] = -1;
            step(true);
            return;
        }
    }
}

void LdProfileController::update(const std::string& zone_name) {
    if (disabled_) return;
    const auto now = std::chrono::steady_clock::now();

    if (!zone_name.empty() && zone_name != target_zone_ && cfg_.profiles.count(zone_name)) {
        if (zone_name != candidate_) {
            candidate_ = zone_name;
            candidate_since_ = now;
        }
        const auto held = std::chrono::duration_cast<std::chrono::seconds>(now - candidate_since_).count();
        // The first profile (daemon start) applies at once; later changes wait out the dwell.
        const bool first = target_zone_.empty();
        if (first || held >= cfg_.min_dwell_sec) {
            LOG_INFO("ld_profile", "Zone '" << zone_name << "' -> profile "
                     << describe(cfg_.profiles.at(zone_name).value)
                     << (first ? "" : " (was '" + target_zone_ + "', ramping)"));
            target_zone_ = zone_name;
            candidate_.clear();
            for (int k = 0; k < LD_REGISTER_COUNT; ++k) target_[k] = cfg_.profiles.at(zone_name).value[k];
            settled_logged_ = first;
            step(first);
            last_verify_ = now;
            return;
        }
    } else if (zone_name == target_zone_) {
        candidate_.clear();                                // brief excursion ended before the dwell
    }

    if (target_zone_.empty()) return;
    step(false);
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_verify_).count() >=
            cfg_.verify_interval_sec) {
        last_verify_ = now;
        verify();
    }
}

}  // namespace als_dimmer
