#ifndef ALS_DIMMER_THERMAL_LIMIT_HPP
#define ALS_DIMMER_THERMAL_LIMIT_HPP

#include "als-dimmer/config.hpp"
#include "als-dimmer/thermal_compensation.hpp"
#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <deque>

namespace als_dimmer {

// Thermal limiter: keeps the backlight temperature at about start_c by lowering the brightness
// (and suppressing the local-dimming boost while hot).
//
// Proportional law, evaluated once per NEW temperature reading (poller sequence number, so the
// limiter's and the poller's clocks cannot alias):
//   T      = mean of the last two plausible readings (valid_min_c..valid_max_c; others ignored),
//            so one bad read never acts, and the first cut needs two hot readings
//   target = clamp(100 - gain_percent_per_c * (T - start_c), min_percent, 100)
//   limit  moves towards target by at most step_percent per reading (slew limit)
// The plant is slow (backlight NTC: ~9 C between idle UI and full white at the cap, tau ~15 min on
// the 14.6" direct, review v2 §5), so a pure integrator with a hysteresis band wound down to the
// floor and stayed there; the proportional law settles where the temperature balances the
// brightness and recovers by itself when the room cools.
//
// No (or a stale) reading = no limiting: the limit recovers to 100 % and the boost is released
// (fail-soft; a dead sensor must not dim the display).
class ThermalLimiter {
public:
    ThermalLimiter(const ThermalLimitConfig& cfg, int poll_interval_sec);

    bool enabled() const { return cfg_.enabled; }

    // Call once per control-loop iteration.
    void update(const ThermalCompensation& thermal);

    // Brightness limit in percent (100 = no limiting).
    int factorPercent() const { return factor_; }
    int apply(int brightness) const { return (brightness * factor_ + 50) / 100; }
    bool boostSuppressed() const { return boost_suppressed_; }

    nlohmann::json status(const ThermalCompensation& thermal) const;

private:
    ThermalLimitConfig cfg_;
    int poll_interval_sec_;
    int factor_ = 100;
    int target_ = 100;
    bool boost_suppressed_ = false;
    uint64_t last_seq_ = 0;
    std::deque<double> window_;                 // last plausible readings (max 2)
    double t_mean_ = 0.0;
    bool have_mean_ = false;
    uint64_t rejected_ = 0;
    std::chrono::steady_clock::time_point last_stale_step_;
};

}  // namespace als_dimmer

#endif  // ALS_DIMMER_THERMAL_LIMIT_HPP
