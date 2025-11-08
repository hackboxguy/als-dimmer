#include "als-dimmer/state_manager.hpp"
#include "als-dimmer/logger.hpp"
#include "json.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <libgen.h>
#include <cstring>

using json = nlohmann::json;

namespace als_dimmer {

StateManager::StateManager(const std::string& state_file_path)
    : file_path_(state_file_path), dirty_(false) {
}

bool StateManager::load() {
    std::ifstream file(file_path_);
    if (!file.is_open()) {
        LOG_DEBUG("StateManager", "State file not found, using defaults");
        return false;  // Not an error, will create on first save
    }

    try {
        json j;
        file >> j;

        if (j.contains("version")) {
            state_.version = j["version"].get<int>();
        }

        if (j.contains("mode")) {
            state_.mode = stringToMode(j["mode"].get<std::string>());
        }

        if (j.contains("manual_brightness")) {
            state_.manual_brightness = j["manual_brightness"].get<int>();
        }

        if (j.contains("last_auto_brightness")) {
            state_.last_auto_brightness = j["last_auto_brightness"].get<int>();
        }

        if (j.contains("brightness_offset")) {
            state_.brightness_offset = j["brightness_offset"].get<int>();
        }

        if (j.contains("last_updated")) {
            state_.last_updated = j["last_updated"].get<std::string>();
        }

        LOG_DEBUG("StateManager", "State loaded: mode=" << modeToString(state_.mode)
                  << ", manual_brightness=" << state_.manual_brightness);

        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("StateManager", "Error loading state: " << e.what());
        LOG_WARN("StateManager", "Using default state");
        return false;
    }
}

bool StateManager::save() {
    // Create directory if it doesn't exist
    char* path_copy = strdup(file_path_.c_str());
    char* dir = dirname(path_copy);
    mkdir(dir, 0755);
    free(path_copy);

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%SZ");
    state_.last_updated = oss.str();

    // Create JSON
    json j;
    j["version"] = state_.version;
    j["mode"] = modeToString(state_.mode);
    j["manual_brightness"] = state_.manual_brightness;
    j["last_auto_brightness"] = state_.last_auto_brightness;
    j["brightness_offset"] = state_.brightness_offset;
    j["last_updated"] = state_.last_updated;

    // Write to file
    std::ofstream file(file_path_);
    if (!file.is_open()) {
        LOG_ERROR("StateManager", "Cannot write to state file: " << file_path_);
        return false;
    }

    file << j.dump(2) << "\n";
    dirty_ = false;
    last_save_time_ = std::chrono::steady_clock::now();

    LOG_DEBUG("StateManager", "State saved to " << file_path_);
    return true;
}

void StateManager::setState(const PersistentState& state) {
    state_ = state;
    dirty_ = true;
}

void StateManager::setMode(OperatingMode mode) {
    if (state_.mode != mode) {
        state_.mode = mode;
        dirty_ = true;
        LOG_DEBUG("StateManager", "Mode changed to: " << modeToString(mode));
    }
}

void StateManager::setManualBrightness(int brightness) {
    if (state_.manual_brightness != brightness) {
        state_.manual_brightness = brightness;
        dirty_ = true;
    }
}

void StateManager::setLastAutoBrightness(int brightness) {
    if (state_.last_auto_brightness != brightness) {
        state_.last_auto_brightness = brightness;
        dirty_ = true;
    }
}

void StateManager::markDirty() {
    dirty_ = true;
}

std::string StateManager::modeToString(OperatingMode mode) {
    switch (mode) {
        case OperatingMode::AUTO:
            return "auto";
        case OperatingMode::MANUAL:
            return "manual";
        case OperatingMode::MANUAL_TEMPORARY:
            return "manual_temporary";
        default:
            return "unknown";
    }
}

OperatingMode StateManager::stringToMode(const std::string& str) {
    if (str == "auto") {
        return OperatingMode::AUTO;
    } else if (str == "manual") {
        return OperatingMode::MANUAL;
    } else if (str == "manual_temporary") {
        return OperatingMode::MANUAL_TEMPORARY;
    } else {
        LOG_WARN("StateManager", "Unknown mode: " << str << ", defaulting to AUTO");
        return OperatingMode::AUTO;
    }
}

} // namespace als_dimmer
