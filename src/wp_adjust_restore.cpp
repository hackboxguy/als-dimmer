#include "als-dimmer/wp_adjust_restore.hpp"
#include "als-dimmer/logger.hpp"
#include "json.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

using json = nlohmann::json;

namespace als_dimmer {
namespace {

// wp_adjust scalar v1 logical register map (fpga-wp-adjust
// docs/register-map.md, revision 0x0113).
constexpr uint16_t WP_ADJUST_ID      = 0x57A1;
constexpr uint8_t  WP_VERSION_MAJOR  = 0x01;
constexpr uint16_t COMMIT_MAGIC      = 0xCA1B;

constexpr int REG_CONTROL_SHADOW = 0x00;
constexpr int REG_GAIN_SHADOW[3] = {0x01, 0x02, 0x03};   // R, G, B
constexpr int REG_OFFSET_SHADOW[3] = {0x04, 0x05, 0x06}; // R, G, B
constexpr int REG_ID      = 0x70;
constexpr int REG_VERSION = 0x71;
constexpr int REG_STATUS  = 0x72;
constexpr int REG_COMMIT  = 0x7E;

constexpr uint16_t STATUS_COMMIT_PENDING  = 0x0001;
constexpr uint16_t STATUS_COMMIT_CONSUMED = 0x0002;

const char* const CHANNEL_NAMES[3] = {"r", "g", "b"};

// Minimal i2c-dev access using the wp_adjust page transport: 2 bytes per
// logical register, big-endian, byte_addr = logical << 1, explicit
// {page, reg} pointer on every access (a single-byte pointer would default
// to page 0 on the new slave).
class WpAdjustBus {
public:
    WpAdjustBus() : fd_(-1), page_(0) {}

    ~WpAdjustBus() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    bool open_bus(const std::string& device, int address, int page) {
        page_ = page;
        fd_ = open(device.c_str(), O_RDWR);
        if (fd_ < 0) {
            LOG_WARN("wp_adjust", "Cannot open " << device << ": "
                     << strerror(errno));
            return false;
        }
        if (ioctl(fd_, I2C_SLAVE, address) < 0) {
            LOG_WARN("wp_adjust", "Cannot select I2C slave 0x" << std::hex
                     << address << std::dec << " on " << device << ": "
                     << strerror(errno));
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    bool read16(int logical_reg, uint16_t& value) {
        uint8_t pointer[2] = {
            static_cast<uint8_t>(page_),
            static_cast<uint8_t>((logical_reg << 1) & 0xFF)
        };
        if (write(fd_, pointer, sizeof(pointer)) !=
                static_cast<ssize_t>(sizeof(pointer))) {
            return false;
        }
        uint8_t data[2] = {0, 0};
        if (read(fd_, data, sizeof(data)) !=
                static_cast<ssize_t>(sizeof(data))) {
            return false;
        }
        value = static_cast<uint16_t>((data[0] << 8) | data[1]);
        return true;
    }

    bool write16(int logical_reg, uint16_t value) {
        uint8_t payload[4] = {
            static_cast<uint8_t>(page_),
            static_cast<uint8_t>((logical_reg << 1) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        return write(fd_, payload, sizeof(payload)) ==
               static_cast<ssize_t>(sizeof(payload));
    }

private:
    int fd_;
    int page_;
};

struct WpAdjustProfile {
    uint16_t gains[3] = {0, 0, 0};        // R, G, B (Q-format raw)
    int16_t offsets[3] = {0, 0, 0};       // R, G, B (signed output-code)
    bool offsets_enabled = false;
    int frac_bits = 12;
};

bool parseProfile(const std::string& path, WpAdjustProfile& profile) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("wp_adjust", "Failed to open calibration file " << path
                 << ": " << strerror(errno));
        return false;
    }

    json data;
    try {
        file >> data;
    } catch (const json::parse_error& e) {
        LOG_WARN("wp_adjust", "Calibration JSON parse error in " << path
                 << ": " << e.what());
        return false;
    }

    try {
        if (!data.is_object() || !data.contains("gains")) {
            LOG_WARN("wp_adjust", "Calibration file " << path
                     << " has no gains object; skipping");
            return false;
        }
        const json& gains = data["gains"];
        for (int i = 0; i < 3; ++i) {
            const char* key = CHANNEL_NAMES[i];
            if (!gains.contains(key) || !gains[key].is_number_integer()) {
                LOG_WARN("wp_adjust", "Calibration gains." << key
                         << " missing or not an integer in " << path);
                return false;
            }
            long value = gains[key].get<long>();
            if (value < 0 || value > 0xFFFF) {
                LOG_WARN("wp_adjust", "Calibration gains." << key
                         << " out of uint16 range in " << path);
                return false;
            }
            profile.gains[i] = static_cast<uint16_t>(value);
        }

        if (data.contains("fpga") && data["fpga"].is_object() &&
            data["fpga"].contains("frac_bits") &&
            data["fpga"]["frac_bits"].is_number_integer()) {
            profile.frac_bits = data["fpga"]["frac_bits"].get<int>();
            if (profile.frac_bits < 1 || profile.frac_bits > 15) {
                LOG_WARN("wp_adjust", "Calibration fpga.frac_bits out of "
                         "range in " << path);
                return false;
            }
        }

        if (data.contains("offsets") && data["offsets"].is_object()) {
            const json& offsets = data["offsets"];
            if (offsets.contains("enabled") && offsets["enabled"].is_boolean()) {
                profile.offsets_enabled = offsets["enabled"].get<bool>();
            }
            for (int i = 0; i < 3; ++i) {
                const char* key = CHANNEL_NAMES[i];
                if (offsets.contains(key) && offsets[key].is_number_integer()) {
                    long value = offsets[key].get<long>();
                    if (value < -32768 || value > 32767) {
                        LOG_WARN("wp_adjust", "Calibration offsets." << key
                                 << " out of int16 range in " << path);
                        return false;
                    }
                    profile.offsets[i] = static_cast<int16_t>(value);
                }
            }
        }
    } catch (const json::exception& e) {
        LOG_WARN("wp_adjust", "Calibration JSON field error in " << path
                 << ": " << e.what());
        return false;
    }

    return true;
}

// Boot-safe plausibility window, mirroring fpga-wp-adjust host/wp_load.py:
// raw gains in [unity/4, unity] so a corrupt-but-parseable profile cannot
// black out or blow out the display from an unattended boot path.
bool gainsInBootWindow(const WpAdjustProfile& profile) {
    const int unity = 1 << profile.frac_bits;
    const int min_raw = unity / 4;
    for (int i = 0; i < 3; ++i) {
        if (profile.gains[i] < min_raw || profile.gains[i] > unity) {
            LOG_WARN("wp_adjust", "Calibration gain " << CHANNEL_NAMES[i]
                     << "=0x" << std::hex << profile.gains[i] << std::dec
                     << " outside boot-safe window [unity/4, unity]; "
                     "not applying");
            return false;
        }
    }
    return true;
}

} // namespace

WpAdjustRestoreStatus restoreWpAdjustCalibration(
    const WpAdjustCalibrationConfig& cfg,
    const std::string& fallback_i2c_device) {
    if (!cfg.enabled) {
        // Default-off gate: not opted in, so send no I2C traffic and let the
        // caller run the legacy wpx/wpy/wpz path (Lattice / legacy displays).
        return WpAdjustRestoreStatus::NotPresent;
    }

    const std::string device =
        cfg.i2c_device.empty() ? fallback_i2c_device : cfg.i2c_device;
    if (device.empty()) {
        LOG_WARN("wp_adjust", "No I2C device configured for wp_adjust restore "
                 "(set white_point_calibration.wp_adjust.i2c_device); "
                 "deferring to legacy white-point restore");
        return WpAdjustRestoreStatus::NotPresent;
    }

    WpAdjustBus bus;
    if (!bus.open_bus(device, cfg.i2c_address, cfg.page)) {
        // Cannot open/select the bus -> cannot tell new from legacy; defer to
        // the legacy path (it self-gates on the output type).
        return WpAdjustRestoreStatus::NotPresent;
    }

    // --- Presence probe FIRST, before touching the calibration file. ---
    // A new-generation pixelpipe display answers the wp_adjust ID on the new
    // slave (and ALSO exposes the legacy 0x1D slave); a legacy display answers
    // only on 0x1D and NACKs here. So the wp_adjust ID is the new-vs-legacy
    // discriminator: present => new path owns white point; absent => legacy.
    uint16_t id = 0, version = 0, status = 0;
    if (!bus.read16(REG_ID, id) || id != WP_ADJUST_ID) {
        LOG_INFO("wp_adjust", "wp_adjust not detected on " << device
                 << " addr 0x" << std::hex << cfg.i2c_address << " page 0x"
                 << cfg.page << std::dec
                 << "; deferring to legacy white-point restore");
        return WpAdjustRestoreStatus::NotPresent;
    }

    // From here the wp_adjust block IS present (pixelpipe display); the new path
    // owns this display's white point. EVERY exit below returns Present so the
    // caller does NOT also run the legacy wpx/wpy/wpz restore - even when the
    // calibration file is missing/invalid (the display just stays in wp_adjust
    // pass-through / unity).
    if (!bus.read16(REG_VERSION, version) ||
        ((version >> 8) & 0xFF) != WP_VERSION_MAJOR) {
        LOG_WARN("wp_adjust", "Unsupported wp_adjust register-map version 0x"
                 << std::hex << version << std::dec
                 << "; not applying (display in pass-through)");
        return WpAdjustRestoreStatus::Present;
    }

    // Load + validate the calibration file. Missing/invalid is non-fatal: the
    // display stays in wp_adjust pass-through, still not the legacy path.
    if (cfg.file_path.empty()) {
        LOG_WARN("wp_adjust", "wp_adjust present but file_path is empty; "
                 "display left in pass-through");
        return WpAdjustRestoreStatus::Present;
    }
    if (access(cfg.file_path.c_str(), F_OK) != 0) {
        if (errno == ENOENT) {
            LOG_INFO("wp_adjust", "No wp_adjust calibration file at "
                     << cfg.file_path << "; display in pass-through");
        } else {
            LOG_WARN("wp_adjust", "Cannot access wp_adjust calibration file "
                     << cfg.file_path << ": " << strerror(errno));
        }
        return WpAdjustRestoreStatus::Present;
    }

    WpAdjustProfile profile;
    if (!parseProfile(cfg.file_path, profile)) {
        return WpAdjustRestoreStatus::Present;
    }
    if (!gainsInBootWindow(profile)) {
        return WpAdjustRestoreStatus::Present;
    }

    if (!bus.read16(REG_STATUS, status)) {
        LOG_WARN("wp_adjust", "STATUS read failed; not applying");
        return WpAdjustRestoreStatus::Present;
    }
    if (((status >> 8) & 0xFF) != profile.frac_bits) {
        LOG_WARN("wp_adjust", "FRAC_BITS mismatch: device reports "
                 << ((status >> 8) & 0xFF) << ", calibration expects "
                 << profile.frac_bits << "; not applying");
        return WpAdjustRestoreStatus::Present;
    }
    if (status & STATUS_COMMIT_PENDING) {
        LOG_WARN("wp_adjust", "A commit is already pending; not applying");
        return WpAdjustRestoreStatus::Present;
    }

    // Shadow writes + readback verify (a lost/corrupted I2C write must not
    // be committed), then the frame-boundary COMMIT.
    const uint16_t control =
        static_cast<uint16_t>(0x0001 | (profile.offsets_enabled ? 0x0002 : 0x0000));
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i) {
        ok = bus.write16(REG_GAIN_SHADOW[i], profile.gains[i]);
    }
    for (int i = 0; i < 3 && ok; ++i) {
        ok = bus.write16(REG_OFFSET_SHADOW[i],
                         static_cast<uint16_t>(profile.offsets[i]));
    }
    ok = ok && bus.write16(REG_CONTROL_SHADOW, control);
    if (!ok) {
        LOG_WARN("wp_adjust", "Shadow register write failed; not committing");
        return WpAdjustRestoreStatus::Present;
    }

    for (int i = 0; i < 3; ++i) {
        uint16_t readback = 0;
        if (!bus.read16(REG_GAIN_SHADOW[i], readback) ||
            readback != profile.gains[i]) {
            LOG_WARN("wp_adjust", "Shadow readback mismatch on gain "
                     << CHANNEL_NAMES[i] << " (wrote 0x" << std::hex
                     << profile.gains[i] << ", read 0x" << readback
                     << std::dec << "); not committing");
            return WpAdjustRestoreStatus::Present;
        }
    }
    for (int i = 0; i < 3; ++i) {
        uint16_t readback = 0;
        const uint16_t want = static_cast<uint16_t>(profile.offsets[i]);
        if (!bus.read16(REG_OFFSET_SHADOW[i], readback) || readback != want) {
            LOG_WARN("wp_adjust", "Shadow readback mismatch on offset "
                     << CHANNEL_NAMES[i] << " (wrote 0x" << std::hex << want
                     << ", read 0x" << readback << std::dec
                     << "); not committing");
            return WpAdjustRestoreStatus::Present;
        }
    }
    {
        uint16_t readback = 0;
        if (!bus.read16(REG_CONTROL_SHADOW, readback) || readback != control) {
            LOG_WARN("wp_adjust", "Shadow readback mismatch on CONTROL; "
                     "not committing");
            return WpAdjustRestoreStatus::Present;
        }
    }

    if (!bus.write16(REG_COMMIT, COMMIT_MAGIC)) {
        LOG_WARN("wp_adjust", "COMMIT write failed");
        return WpAdjustRestoreStatus::Present;
    }

    // Poll for commit-consumed. Video may not be running yet at daemon
    // start; a still-pending commit is SUCCESS ("pending until video") -
    // it latches at the first vsync.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg.commit_timeout_ms);
    while (true) {
        if (!bus.read16(REG_STATUS, status)) {
            LOG_WARN("wp_adjust", "STATUS poll failed after COMMIT");
            return WpAdjustRestoreStatus::Present;
        }
        if (status & STATUS_COMMIT_CONSUMED) {
            LOG_INFO("wp_adjust", "Applied wp_adjust calibration from "
                     << cfg.file_path << ": R=0x" << std::hex
                     << profile.gains[0] << " G=0x" << profile.gains[1]
                     << " B=0x" << profile.gains[2] << std::dec
                     << " (committed)");
            return WpAdjustRestoreStatus::Present;
        }
        if (!(status & STATUS_COMMIT_PENDING)) {
            LOG_INFO("wp_adjust", "wp_adjust calibration written; commit "
                     "state unknown (STATUS idle)");
            return WpAdjustRestoreStatus::Present;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            LOG_INFO("wp_adjust", "wp_adjust calibration written; commit "
                     "pending until video starts");
            return WpAdjustRestoreStatus::Present;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace als_dimmer
