#include "als-dimmer/thermal_limit.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace als_dimmer {

ThermalLimiter::ThermalLimiter(const ThermalLimitConfig& cfg, int poll_interval_sec)
    : cfg_(cfg), poll_interval_sec_(std::max(1, poll_interval_sec)) {
    if (cfg_.enabled) {
        LOG_INFO("thermal_limit", "Thermal limiter enabled: start " << cfg_.start_c << " C, "
                 << cfg_.gain_percent_per_c << " %/C above it, slew " << cfg_.step_percent
                 << " % per reading, floor " << cfg_.min_percent << " %"
                 << (cfg_.suppress_boost ? ", LD boost off while hot" : ""));
    }
}

void ThermalLimiter::update(const ThermalCompensation& thermal) {
    if (!cfg_.enabled) return;
    const int old_factor = factor_;
    const bool old_suppressed = boost_suppressed_;
    const auto now = std::chrono::steady_clock::now();

    if (!thermal.hasReading()) {
        // No or stale reading: recover at the slew rate, release the boost.
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stale_step_).count() >= poll_interval_sec_) {
            last_stale_step_ = now;
            window_.clear();
            have_mean_ = false;
            target_ = 100;
            factor_ = std::min(100, factor_ + cfg_.step_percent);
            if (factor_ == 100) boost_suppressed_ = false;
        }
    } else {
        const uint64_t seq = thermal.readSeq();
        if (seq == last_seq_) return;                     // act once per new reading
        last_seq_ = seq;
        const double t = thermal.lastTempC();
        if (!std::isfinite(t) || t < cfg_.valid_min_c || t > cfg_.valid_max_c) {
            if (rejected_++ % 30 == 0) {
                LOG_WARN("thermal_limit", "Ignoring implausible backlight temperature " << t << " C");
            }
            return;
        }
        window_.push_back(t);
        while (window_.size() > 2) window_.pop_front();
        if (window_.size() < 2) return;                   // two readings before acting
        t_mean_ = (window_[0] + window_[1]) / 2.0;
        have_mean_ = true;

        const double over = t_mean_ - cfg_.start_c;
        target_ = over <= 0.0 ? 100
                : std::max(cfg_.min_percent,
                           static_cast<int>(std::lround(100.0 - cfg_.gain_percent_per_c * over)));
        // 1 % deadband: the mean of two 0.1 C readings makes the target flip by 1 % at a constant
        // temperature (review v3 N11); a 1 % brightness step every 10 s would be visible. Moving
        // back to 100 % is always allowed.
        if (std::abs(target_ - factor_) > 1 || target_ == 100) {
            if (factor_ > target_) factor_ = std::max(target_, factor_ - cfg_.step_percent);
            else if (factor_ < target_) factor_ = std::min(target_, factor_ + cfg_.step_percent);
        }

        if (cfg_.suppress_boost) {
            if (t_mean_ >= cfg_.start_c) boost_suppressed_ = true;
            else if (t_mean_ < cfg_.start_c - cfg_.hysteresis_c && factor_ == 100) boost_suppressed_ = false;
        }
    }

    if (factor_ != old_factor || boost_suppressed_ != old_suppressed) {
        std::ostringstream t;
        if (have_mean_) t << std::fixed << std::setprecision(1) << t_mean_ << " C";
        else t << "temperature unknown";
        LOG_INFO("thermal_limit", "Backlight " << t.str() << ": brightness limit " << factor_
                 << " % (target " << target_ << " %)" << (boost_suppressed_ ? ", LD boost suppressed" : ""));
    }
}

nlohmann::json ThermalLimiter::status(const ThermalCompensation& thermal) const {
    nlohmann::json j;
    j["enabled"] = cfg_.enabled;
    j["start_c"] = cfg_.start_c;
    j["gain_percent_per_c"] = cfg_.gain_percent_per_c;
    j["brightness_limit_percent"] = factor_;
    j["target_percent"] = target_;
    j["boost_suppressed"] = boost_suppressed_;
    j["limiting"] = factor_ < 100 || boost_suppressed_;
    j["rejected_readings"] = rejected_;
    if (thermal.hasReading()) {
        j["backlight_temp_c"] = thermal.lastTempC();
    } else {
        j["backlight_temp_c"] = nullptr;
    }
    if (have_mean_) j["backlight_temp_mean_c"] = t_mean_;
    else j["backlight_temp_mean_c"] = nullptr;
    return j;
}

}  // namespace als_dimmer
