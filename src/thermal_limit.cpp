#include "als-dimmer/thermal_limit.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>

namespace als_dimmer {

ThermalLimiter::ThermalLimiter(const ThermalLimitConfig& cfg, int poll_interval_sec)
    : cfg_(cfg), poll_interval_sec_(std::max(1, poll_interval_sec)) {
    if (cfg_.enabled) {
        LOG_INFO("thermal_limit", "Thermal limiter enabled: start " << cfg_.start_c << " C, hysteresis "
                 << cfg_.hysteresis_c << " C, step " << cfg_.step_percent << " % per "
                 << poll_interval_sec_ << " s, floor " << cfg_.min_percent << " %"
                 << (cfg_.suppress_boost ? ", LD boost off first" : ""));
    }
}

void ThermalLimiter::update(const ThermalCompensation& thermal) {
    if (!cfg_.enabled) return;
    const auto now = std::chrono::steady_clock::now();
    if (evaluated_ &&
        std::chrono::duration_cast<std::chrono::seconds>(now - last_eval_).count() < poll_interval_sec_) {
        return;
    }
    evaluated_ = true;
    last_eval_ = now;

    const int old_factor = factor_;
    const bool old_suppressed = boost_suppressed_;
    if (!thermal.hasReading()) {
        factor_ = std::min(100, factor_ + cfg_.step_percent);   // no reading: no limiting
    } else {
        const double t = thermal.lastTempC();
        if (t >= cfg_.start_c) {
            if (cfg_.suppress_boost) boost_suppressed_ = true;
            factor_ = std::max(cfg_.min_percent, factor_ - cfg_.step_percent);
        } else if (t <= cfg_.start_c - cfg_.hysteresis_c) {
            factor_ = std::min(100, factor_ + cfg_.step_percent);
        }
        if (factor_ == 100 && t <= cfg_.start_c - cfg_.hysteresis_c) boost_suppressed_ = false;
    }
    if (!thermal.hasReading() && factor_ == 100) boost_suppressed_ = false;

    if (factor_ != old_factor || boost_suppressed_ != old_suppressed) {
        LOG_INFO("thermal_limit", "Backlight "
                 << (thermal.hasReading() ? std::to_string(thermal.lastTempC()).substr(0, 5) + " C" : "temperature unknown")
                 << ": brightness limit " << factor_ << " %"
                 << (boost_suppressed_ ? ", LD boost suppressed" : ""));
    }
}

nlohmann::json ThermalLimiter::status(const ThermalCompensation& thermal) const {
    nlohmann::json j;
    j["enabled"] = cfg_.enabled;
    j["start_c"] = cfg_.start_c;
    j["hysteresis_c"] = cfg_.hysteresis_c;
    j["brightness_limit_percent"] = factor_;
    j["boost_suppressed"] = boost_suppressed_;
    j["limiting"] = factor_ < 100 || boost_suppressed_;
    if (thermal.hasReading()) {
        j["backlight_temp_c"] = thermal.lastTempC();
    } else {
        j["backlight_temp_c"] = nullptr;
    }
    return j;
}

}  // namespace als_dimmer
