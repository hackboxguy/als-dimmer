#ifndef ALS_DIMMER_THERMAL_LIMIT_HPP
#define ALS_DIMMER_THERMAL_LIMIT_HPP

#include "als-dimmer/config.hpp"
#include "als-dimmer/thermal_compensation.hpp"
#include "json.hpp"

#include <chrono>

namespace als_dimmer {

// Thermal limiter: keeps the backlight temperature at or below start_c by lowering the
// brightness (and, first, suppressing the local-dimming boost). A slow integral regulator on the
// poller's readings: each new reading at or above start_c lowers the factor by step_percent (floor
// min_percent); each reading at or below start_c - hysteresis_c raises it by step_percent (up to
// 100 %); in between it holds. The boost is suppressed from the first hot reading until the factor
// is back at 100 % and the temperature is below the hysteresis band.
//
// No (or a stale) reading = no limiting: the factor recovers to 100 % and the boost is released,
// like the rest of the thermal code (fail-soft; a dead sensor must not dim the display).
class ThermalLimiter {
public:
    ThermalLimiter(const ThermalLimitConfig& cfg, int poll_interval_sec);

    bool enabled() const { return cfg_.enabled; }

    // Call once per control-loop iteration. Acts at most once per poll interval.
    void update(const ThermalCompensation& thermal);

    // Brightness factor in percent (100 = no limiting).
    int factorPercent() const { return factor_; }
    int apply(int brightness) const { return (brightness * factor_ + 50) / 100; }
    bool boostSuppressed() const { return boost_suppressed_; }

    nlohmann::json status(const ThermalCompensation& thermal) const;

private:
    ThermalLimitConfig cfg_;
    int poll_interval_sec_;
    int factor_ = 100;
    bool boost_suppressed_ = false;
    std::chrono::steady_clock::time_point last_eval_;
    bool evaluated_ = false;
};

}  // namespace als_dimmer

#endif  // ALS_DIMMER_THERMAL_LIMIT_HPP
