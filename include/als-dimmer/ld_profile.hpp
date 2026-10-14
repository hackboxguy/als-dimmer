#ifndef ALS_DIMMER_LD_PROFILE_HPP
#define ALS_DIMMER_LD_PROFILE_HPP

#include "als-dimmer/config.hpp"

#include <chrono>
#include <string>

namespace als_dimmer {

// Day/night local-dimming profile for pixelpipe FPGA displays (step 3 of
// pixelpipe-fpga docs/dynamic-local-dimming.md).
//
// The zone the ALS loop detects (zone_mapper: e.g. night / indoor / outdoor,
// with its hysteresis) selects a profile of FPGA LD registers on the new I2C
// slave (0x1E, page 0): LD alpha 0x31, PC tune 0x33, LED gain 0x43, low-APL
// boost 0x49, boost rise 0x4A. A zone must stay the detected zone for
// min_dwell_sec before it becomes the target, so a passing shadow or headlight
// does not flip it. The zone follows the ambient light in AUTO and MANUAL
// brightness modes alike (halo vs readability depends on the room, not on who
// set the brightness).
//
// The registers move to the new profile gradually: at most transition_step[k]
// per control-loop iteration (the FPGA applies a lowered boost at once, so a
// direct write would be a visible brightness step). A step of 0 = write at once.
//
// The registers are re-read every verify_interval_sec and rewritten if they
// differ (FPGA reboot / OTA activation powers them up at their defaults). A
// register that does not hold a written value (bitstream without it) is
// dropped after one warning. Everything is fail-soft: an absent 0x1E slave
// disables the feature with one log line.
class LdProfileController {
public:
    LdProfileController(const LdProfileConfig& cfg, const std::string& fallback_i2c_device);
    ~LdProfileController();

    // Call once per control-loop iteration with the zone the ALS loop detected
    // ("" when the sensor is unavailable: the current target is kept).
    void update(const std::string& zone_name);

    const std::string& activeProfile() const { return target_zone_; }

private:
    bool openBus();
    bool writeReg(int reg, int value);
    bool readReg(int reg, int& value);
    void step(bool first);
    void verify();

    LdProfileConfig cfg_;
    std::string device_;
    int fd_ = -1;
    bool disabled_ = false;
    std::string target_zone_;                    // profile being applied / applied
    std::string candidate_;                      // zone waiting out the dwell time
    std::chrono::steady_clock::time_point candidate_since_;
    std::chrono::steady_clock::time_point last_verify_;
    int current_[LD_REGISTER_COUNT];             // last value written (-1 = never)
    int target_[LD_REGISTER_COUNT];              // -1 = not part of the profile
    int unsupported_mask_ = 0;                   // bit per LdRegister index
    bool settled_logged_ = true;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_LD_PROFILE_HPP
