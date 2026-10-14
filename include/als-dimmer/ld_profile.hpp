#ifndef ALS_DIMMER_LD_PROFILE_HPP
#define ALS_DIMMER_LD_PROFILE_HPP

#include "als-dimmer/config.hpp"
#include "json.hpp"

#include <chrono>
#include <string>

namespace als_dimmer {

// Day/night local-dimming profile for pixelpipe FPGA displays (step 3 of
// pixelpipe-fpga docs/dynamic-local-dimming.md).
//
// The zone the ALS loop detects (zone_mapper: e.g. night / indoor / outdoor,
// with its hysteresis) selects a profile of FPGA LD registers on the new I2C
// slave (0x1E, page 0): LD alpha 0x31, PC tune 0x33, LED gain 0x43, low-APL
// boost 0x49, boost rise 0x4A. The room must have been in another zone for
// min_dwell_sec before that zone's profile becomes the target, so a passing
// shadow or headlight does not flip it. The zone follows the ambient light in
// AUTO and MANUAL brightness modes alike (halo vs readability depends on the
// room, not on who set the brightness).
//
// Two overrides sit on top of the profile:
//   - dynamic LD off (user switch, persisted by the caller): the static_profile
//     is used whatever the zone;
//   - boost suppressed (thermal limiter): bl_boost is forced to 64 (off).
//
// The registers move to the target gradually: at most transition_step[k] per
// control-loop iteration (0 = write at once; from FPGA v0x57 the FPGA itself
// glides a lowered boost, so bl_boost can be written at once).
//
// The registers are re-read every verify_interval_sec. A mismatch confirmed by
// a second read (FPGA reboot / OTA activation / another tool) re-syncs to what
// the FPGA holds and the ramp continues from there. A register is dropped only
// after it read back 0 after a non-zero write three times in a row (a bitstream
// without it); any other read-back difference is retried. Everything is
// fail-soft: an absent 0x1E slave disables the feature with one log line.
class LdProfileController {
public:
    LdProfileController(const LdProfileConfig& cfg, const std::string& fallback_i2c_device);
    ~LdProfileController();

    bool available() const { return !disabled_; }

    // Call once per control-loop iteration with the zone the ALS loop detected
    // ("" when the sensor is unavailable: the current zone is kept).
    void update(const std::string& zone_name);

    void setDynamicEnabled(bool enabled);
    bool dynamicEnabled() const { return dynamic_enabled_; }
    void setBoostSuppressed(bool suppressed);
    bool boostSuppressed() const { return boost_suppressed_; }

    // "night" / "indoor" / ... or "static" when dynamic LD is off; "" before the first zone.
    std::string activeProfile() const;

    // Live status for the control socket: switch, profile, target/current registers and the
    // FPGA's own backlight status (0x46..0x48 read on demand).
    nlohmann::json status();

private:
    bool openBus();
    bool writeReg(int reg, int value);
    bool readReg(int reg, int& value);
    void computeTarget();
    void step(bool at_once);
    void verify();

    LdProfileConfig cfg_;
    std::string device_;
    int fd_ = -1;
    bool disabled_ = false;
    bool dynamic_enabled_ = true;
    bool boost_suppressed_ = false;
    std::string zone_;                           // zone whose profile is the target ("" = none yet)
    std::chrono::steady_clock::time_point away_since_;
    bool away_ = false;                          // detected zone differs from zone_
    std::chrono::steady_clock::time_point last_verify_;
    int current_[LD_REGISTER_COUNT];             // last value written / read (-1 = unknown)
    int target_[LD_REGISTER_COUNT];              // -1 = not managed
    int zero_reads_[LD_REGISTER_COUNT];          // consecutive "0 after a non-zero write"
    int unsupported_mask_ = 0;                   // bit per LdRegister index
    bool settled_logged_ = true;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_LD_PROFILE_HPP
