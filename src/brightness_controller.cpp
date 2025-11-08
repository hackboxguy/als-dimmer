#include "als-dimmer/brightness_controller.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace als_dimmer {

BrightnessController::BrightnessController() {
    // No initialization needed for now
}

int BrightnessController::calculateNextBrightness(int target_brightness,
                                                   int current_brightness,
                                                   const Zone* zone) const {
    // Calculate error
    int error = target_brightness - current_brightness;

    // If already at target, no change needed
    if (error == 0) {
        return current_brightness;
    }

    // Get appropriate step size based on error magnitude
    int step = getStepSize(error, zone);

    // Calculate next brightness value
    int next_brightness;
    if (std::abs(error) <= step) {
        // Within one step of target, jump directly to avoid oscillation
        next_brightness = target_brightness;
    } else {
        // Take a step in the right direction
        next_brightness = current_brightness + (error > 0 ? step : -step);
    }

    // Clamp to valid range [0, 100]
    return std::max(0, std::min(100, next_brightness));
}

int BrightnessController::getStepSize(int error, const Zone* zone) const {
    int abs_error = std::abs(error);

    // Get thresholds and step sizes from zone, or use defaults
    int threshold_large, threshold_small;
    int step_large, step_medium, step_small;

    if (zone) {
        threshold_large = zone->error_thresholds.large;
        threshold_small = zone->error_thresholds.small;
        step_large = zone->step_sizes.large;
        step_medium = zone->step_sizes.medium;
        step_small = zone->step_sizes.small;
    } else {
        // Simple mode defaults
        threshold_large = DEFAULT_THRESHOLD_LARGE;
        threshold_small = DEFAULT_THRESHOLD_SMALL;
        step_large = DEFAULT_STEP_LARGE;
        step_medium = DEFAULT_STEP_MEDIUM;
        step_small = DEFAULT_STEP_SMALL;
    }

    // Select step size based on error magnitude
    if (abs_error > threshold_large) {
        return step_large;      // Large error: fast convergence
    } else if (abs_error > threshold_small) {
        return step_medium;     // Medium error: moderate convergence
    } else {
        return step_small;      // Small error: fine-tuning
    }
}

} // namespace als_dimmer
