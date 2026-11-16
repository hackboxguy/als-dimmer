#ifndef ALS_DIMMER_NOTIFIER_HPP
#define ALS_DIMMER_NOTIFIER_HPP

#include "config.hpp"
#include <string>
#include <chrono>

namespace als_dimmer {

class Notifier {
public:
    explicit Notifier(const NotificationConfig& config);

    // Emit events — only fires if value actually changed (deduplication).
    // brightness_changed is additionally rate-limited to ~1/second.
    void emitModeChanged(const std::string& mode);
    void emitBrightnessChanged(int brightness);
    void emitZoneChanged(const std::string& zone);
    // Privacy film: "on" / "off" / "unavailable" (the image has no film drive)
    void emitPrivacyChanged(const std::string& state);
    // Privacy-light level: "0".."100" / "unavailable" (no privacy light)
    void emitPrivacyLevelChanged(const std::string& level);

private:
    void invokeScript(const std::string& event_type, const std::string& value);
    bool isEnabled() const;

    NotificationConfig config_;

    // Last-emitted values for deduplication
    std::string last_mode_;
    int last_brightness_ = -1;
    std::string last_zone_;
    std::string last_privacy_;
    std::string last_privacy_level_;

    // Rate limiting for brightness_changed
    std::chrono::steady_clock::time_point last_brightness_emit_time_;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_NOTIFIER_HPP
