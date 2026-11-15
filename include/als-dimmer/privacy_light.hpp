#ifndef ALS_DIMMER_PRIVACY_LIGHT_HPP
#define ALS_DIMMER_PRIVACY_LIGHT_HPP

#include <chrono>
#include <cstdint>
#include <string>

#include "als-dimmer/config.hpp"
#include "json.hpp"

namespace als_dimmer {

/*
 * Privacy-light level on the display IOC (RH850 at 0x66, rh850-baremetal
 * docs/i2c_register_map.md, "Privacy light"):
 *   0x0202 PRIVACY_LEVEL   R/W  0..100 %, RAM only on the MCU (0 after reset)
 *   0x0203 PRIVACY_STATUS  R    0x01 PWM running, 0x00 stopped (display off)
 * Both are reserved on every RH850 target; a board without the light answers
 * 0xFF, so one 2-byte read of 0x0202 tells "supported" from "not" on any
 * display, whatever the brightness output is. display_manager_s4 v01.12+
 * (sp6bins release 1.25) is the first firmware that drives it.
 *
 * The MCU forgets the level at every reset, and an S4 can reset without the
 * Pi rebooting (OTA, display-only power loss). So the daemon owns the level:
 * it writes the saved level when it finds the light, and re-checks it every
 * poll_interval_sec, writing it again when the MCU holds something else.
 *
 * Main-loop thread only (commands are processed there too): no locking.
 */
class PrivacyLight {
public:
    explicit PrivacyLight(const PrivacyLightConfig& cfg);
    ~PrivacyLight();

    PrivacyLight(const PrivacyLight&) = delete;
    PrivacyLight& operator=(const PrivacyLight&) = delete;

    // The level to hold (from the state file). Written at the next poll.
    void setDesired(int level);
    int desired() const { return desired_; }

    // Call every main-loop iteration; does I2C only when a check is due.
    void poll();

    // Set by a client: write now and read back. false = not available or the
    // write did not take (the next poll retries).
    bool apply(int level);

    bool available() const { return available_; }
    bool running() const { return running_; }
    int mcuLevel() const { return mcu_level_; }

    // {"available", "level", "running", "mcu_level", "reachable"}
    nlohmann::json status() const;

private:
    bool readPair(int& level, int& status);
    bool writeLevel(int level);
    bool ensureOpen();
    void closeFd();

    PrivacyLightConfig cfg_;
    int fd_ = -1;
    int desired_ = 0;
    bool available_ = false;   // sticky once found: a display power cycle must not hide the slider
    bool reachable_ = false;   // the last read answered
    bool running_ = false;
    int mcu_level_ = -1;
    int probe_count_ = 0;
    std::chrono::steady_clock::time_point next_check_;
};

}  // namespace als_dimmer

#endif  // ALS_DIMMER_PRIVACY_LIGHT_HPP
