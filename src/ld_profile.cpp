#include "als-dimmer/ld_profile.hpp"
#include "als-dimmer/logger.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {
namespace {

// pixelpipe-fpga new slave (0x1E) page-0 registers, indexed by LdRegister.
const int REG_ADDR[LD_REGISTER_COUNT]         = {0x31, 0x33, 0x43, 0x49, 0x4A};
const char* const REG_NAME[LD_REGISTER_COUNT] = {"ld_alpha", "pc_tune", "led_gain", "bl_boost", "bl_rise"};
constexpr uint8_t PAGE = 0x00;
constexpr int BOOST_OFF = 64;          // 0x49: 64 = 1.0x
constexpr int ZERO_READS_TO_DROP = 3;  // "reads 0 after a non-zero write" this often in a row

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
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        current_[k] = target_[k] = -1;
        zero_reads_[k] = 0;
    }
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
    LOG_INFO("ld_profile", "  static (dynamic LD off): " << describe(cfg_.static_profile.value));
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

// One I2C_RDWR transaction (pointer write + repeated-start read), so another process on the bus
// cannot move the slave's {page, reg} pointer between the two (see wp_adjust_restore.cpp).
bool LdProfileController::readReg(int reg, int& value) {
    uint8_t d = 0;
    if (!readBlock(reg, &d, 1)) return false;
    value = d;
    return true;
}

// Burst read of `len` registers from `reg` in one I2C_RDWR transaction.
bool LdProfileController::readBlock(int reg, uint8_t* data, int len) {
    uint8_t ptr[2] = {PAGE, static_cast<uint8_t>(reg)};
    struct i2c_msg msgs[2];
    msgs[0].addr = static_cast<__u16>(cfg_.i2c_address);
    msgs[0].flags = 0;
    msgs[0].len = sizeof(ptr);
    msgs[0].buf = ptr;
    msgs[1].addr = static_cast<__u16>(cfg_.i2c_address);
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = static_cast<__u16>(len);
    msgs[1].buf = data;
    struct i2c_rdwr_ioctl_data xfer;
    xfer.msgs = msgs;
    xfer.nmsgs = 2;
    return ioctl(fd_, I2C_RDWR, &xfer) >= 0;
}

std::string LdProfileController::activeProfile() const {
    if (!dynamic_enabled_) return "static";
    return zone_;
}

// Target = static profile (dynamic LD off) or the zone's profile, then the thermal override.
void LdProfileController::computeTarget() {
    const LdProfile* p = nullptr;
    if (!dynamic_enabled_) {
        p = &cfg_.static_profile;
    } else {
        auto it = cfg_.profiles.find(zone_);
        if (it != cfg_.profiles.end()) p = &it->second;
    }
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) target_[k] = p ? p->value[k] : -1;
    if (boost_suppressed_) target_[BL_BOOST] = BOOST_OFF;
    settled_logged_ = false;
}

void LdProfileController::setDynamicEnabled(bool enabled) {
    if (enabled == dynamic_enabled_) return;
    dynamic_enabled_ = enabled;
    LOG_INFO("ld_profile", "Dynamic local dimming " << (enabled ? "ON" : "OFF")
             << " -> profile '" << activeProfile() << "'");
    if (!disabled_) computeTarget();
}

void LdProfileController::setBoostSuppressed(bool suppressed) {
    if (suppressed == boost_suppressed_) return;
    boost_suppressed_ = suppressed;
    LOG_INFO("ld_profile", "Boost " << (suppressed ? "suppressed (thermal)" : "released"));
    if (!disabled_) computeTarget();
}

// Move every managed register one step towards the target (all the way when `at_once`, or when
// the register's step is 0) and read it back.
void LdProfileController::step(bool at_once) {
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
        if (!at_once && s > 0 && std::abs(target_[k] - current_[k]) > s) {
            next = current_[k] + (target_[k] > current_[k] ? s : -s);
        }
        int rb = -1;
        if (!writeReg(REG_ADDR[k], next) || !readReg(REG_ADDR[k], rb)) {
            settled = false;                               // bus busy / display off: next iteration
            continue;
        }
        if (rb != next) {
            settled = false;
            // An unstored register (bitstream without it) reads 0. Anything else is transport noise.
            if (rb == 0 && next != 0 && ++zero_reads_[k] >= ZERO_READS_TO_DROP) {
                unsupported_mask_ |= (1 << k);
                LOG_WARN("ld_profile", REG_NAME[k] << " (0x" << std::hex << REG_ADDR[k] << std::dec
                         << ") reads back 0 after writing " << next << " " << ZERO_READS_TO_DROP
                         << " times: not in this bitstream, dropped from the profiles");
            } else {
                LOG_DEBUG("ld_profile", REG_NAME[k] << " read back " << rb << " after writing "
                          << next << "; retrying");
            }
            continue;
        }
        zero_reads_[k] = 0;
        current_[k] = next;
        if (next != target_[k]) settled = false;
    }
    if (settled && !settled_logged_) {
        settled_logged_ = true;
        LOG_INFO("ld_profile", "Profile '" << activeProfile() << "' in place: " << describe(target_));
    }
}

// Re-read the managed registers. A difference must be seen twice in a row (one corrupt read must
// not turn a ramp into a step); then the controller takes the FPGA's value and ramps from there.
void LdProfileController::verify() {
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        if (current_[k] < 0 || (unsupported_mask_ & (1 << k))) continue;
        int rb = -1, rb2 = -1;
        if (!readReg(REG_ADDR[k], rb)) return;             // display off: try again later
        if (rb == current_[k]) continue;
        if (!readReg(REG_ADDR[k], rb2) || rb2 != rb) continue;
        LOG_INFO("ld_profile", REG_NAME[k] << " reads " << rb << " instead of " << current_[k]
                 << " (FPGA restarted or another tool wrote it); ramping '" << activeProfile()
                 << "' back from there");
        current_[k] = rb;
        settled_logged_ = false;
    }
}

void LdProfileController::update(const std::string& zone_name) {
    if (disabled_) return;
    const auto now = std::chrono::steady_clock::now();

    if (!zone_name.empty() && cfg_.profiles.count(zone_name)) {
        if (zone_.empty()) {
            // First zone after start: applies at once.
            zone_ = zone_name;
            LOG_INFO("ld_profile", "Zone '" << zone_ << "'"
                     << (dynamic_enabled_ ? "" : " (dynamic LD off: static profile)"));
            computeTarget();
            step(true);
            last_verify_ = now;
            return;
        }
        if (zone_name != zone_) {
            // A zone is adopted once it has itself been detected for min_dwell_sec. If the room
            // keeps flapping between other zones (none holds), the zone seen after 3 x dwell away
            // from the current one is adopted (review v1/v2: neither rule alone covers both).
            if (!away_) { away_ = true; away_since_ = now; }
            if (zone_name != candidate_) { candidate_ = zone_name; candidate_since_ = now; }
            using std::chrono::duration_cast;
            using std::chrono::seconds;
            const auto cand_held = duration_cast<seconds>(now - candidate_since_).count();
            const auto away_held = duration_cast<seconds>(now - away_since_).count();
            if (cand_held >= cfg_.min_dwell_sec || away_held >= 3 * cfg_.min_dwell_sec) {
                LOG_INFO("ld_profile", "Zone '" << zone_name << "' (was '" << zone_ << "')"
                         << (dynamic_enabled_ ? ", ramping to its profile" : "; dynamic LD off"));
                zone_ = zone_name;
                away_ = false;
                computeTarget();
            }
        } else {
            away_ = false;                                 // excursion ended before the dwell
            candidate_.clear();
        }
    }

    if (zone_.empty()) {
        if (dynamic_enabled_) return;                      // no zone yet: nothing to apply
        bool have_target = false;                          // dynamic off: the static profile needs no zone
        for (int k = 0; k < LD_REGISTER_COUNT; ++k) have_target = have_target || target_[k] >= 0;
        if (!have_target) computeTarget();
    }
    step(false);
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_verify_).count() >=
            cfg_.verify_interval_sec) {
        last_verify_ = now;
        verify();
    }
}

double LdProfileController::nitsFactor() const {
    if (disabled_ || cfg_.nits_gain_exponent == 0.0) return 1.0;
    int g = current_[LED_GAIN] > 0 ? current_[LED_GAIN] : target_[LED_GAIN];
    if (g <= 0) return 1.0;
    return std::pow(static_cast<double>(g) / cfg_.nits_gain_ref, cfg_.nits_gain_exponent);
}

nlohmann::json LdProfileController::status() {
    nlohmann::json j;
    j["available"] = !disabled_;
    j["dynamic_enabled"] = dynamic_enabled_;
    j["profile"] = activeProfile();
    j["zone"] = zone_;
    j["boost_suppressed"] = boost_suppressed_;
    j["nits_factor"] = nitsFactor();
    if (disabled_) return j;
    nlohmann::json tgt = nlohmann::json::object(), cur = nlohmann::json::object();
    bool settled = true;
    for (int k = 0; k < LD_REGISTER_COUNT; ++k) {
        if (target_[k] < 0) continue;
        tgt[REG_NAME[k]] = target_[k];
        if (unsupported_mask_ & (1 << k)) { cur[REG_NAME[k]] = nullptr; continue; }
        cur[REG_NAME[k]] = current_[k];
        settled = settled && current_[k] == target_[k];
    }
    j["target"] = tgt;
    j["current"] = cur;
    j["settled"] = settled;
    // FPGA backlight status: 0x46/0x47 multiplier in use (0x800 = 100 % brightness), 0x48 APL.
    // One 3-byte burst from 0x46: the FPGA latches {B, APL} when the pointer lands on 0x46
    // (v0x58), so B cannot tear during a ramp. Before v0x58 the burst still narrows the window.
    // Single-byte reads: the FPGA's I2C slave corrupts a 0x67 byte only when it is not the first
    // byte of a read (review v3 §3: 0 errors in 2 000 single-byte reads of any value, 30 % in
    // bursts). 0x46 latches {B, APL} (v0x58), 0x47 comes from that snapshot, 0x48 re-latches
    // (APL at most one frame newer). Two passes must agree and B must be 12-bit.
    uint8_t st[3] = {0, 0, 0}, st2[3] = {1, 1, 1};
    auto read3 = [this](uint8_t* d) {
        return readBlock(0x46, &d[0], 1) && readBlock(0x47, &d[1], 1) && readBlock(0x48, &d[2], 1);
    };
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        ok = read3(st) && read3(st2) && st[0] == st2[0] && st[1] == st2[1] && st[0] <= 0x0F;
    }
    if (ok) {
        const int b = (st[0] << 8) | st[1];
        const int apl = st[2];
        j["fpga_multiplier"] = b;
        j["fpga_multiplier_percent"] = b * 100.0 / 0x800;
        j["fpga_apl_percent"] = apl * 100.0 / 255;
    } else {
        j["fpga_multiplier"] = nullptr;
        j["fpga_apl_percent"] = nullptr;
    }
    return j;
}

}  // namespace als_dimmer
