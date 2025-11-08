#ifndef ALS_DIMMER_STATE_MANAGER_HPP
#define ALS_DIMMER_STATE_MANAGER_HPP

#include <string>
#include <chrono>

namespace als_dimmer {

enum class OperatingMode {
    AUTO,
    MANUAL,
    MANUAL_TEMPORARY
};

struct PersistentState {
    int version = 1;
    OperatingMode mode = OperatingMode::AUTO;
    int manual_brightness = 50;
    int last_auto_brightness = 50;
    int brightness_offset = 0;
    std::string last_updated;
};

class StateManager {
public:
    explicit StateManager(const std::string& state_file_path);

    // Load state from file
    bool load();

    // Save state to file
    bool save();

    // Get current state
    PersistentState getState() const { return state_; }

    // Update state
    void setState(const PersistentState& state);

    // Get/set mode
    OperatingMode getMode() const { return state_.mode; }
    void setMode(OperatingMode mode);

    // Get/set manual brightness
    int getManualBrightness() const { return state_.manual_brightness; }
    void setManualBrightness(int brightness);

    // Get/set last auto brightness
    int getLastAutoBrightness() const { return state_.last_auto_brightness; }
    void setLastAutoBrightness(int brightness);

    // Mark state as dirty (needs save)
    void markDirty();

    // Check if state needs saving
    bool isDirty() const { return dirty_; }

    // Convert mode to string
    static std::string modeToString(OperatingMode mode);
    static OperatingMode stringToMode(const std::string& str);

private:
    std::string file_path_;
    PersistentState state_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_save_time_;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_STATE_MANAGER_HPP
