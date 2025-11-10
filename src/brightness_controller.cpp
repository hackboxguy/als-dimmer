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

BrightnessController::TransitionInfo BrightnessController::calculateNextBrightnessWithInfo(
    int target_brightness,
    int current_brightness,
    const Zone* zone) const {

    TransitionInfo info;
    info.error = target_brightness - current_brightness;

    // Get thresholds and step sizes
    int abs_error = std::abs(info.error);
    bool brightening = (info.error > 0);

    // Get thresholds from zone or use defaults
    if (zone) {
        info.step_threshold_large = zone->error_thresholds.large;
        info.step_threshold_small = zone->error_thresholds.small;
    } else {
        info.step_threshold_large = DEFAULT_THRESHOLD_LARGE;
        info.step_threshold_small = DEFAULT_THRESHOLD_SMALL;
    }

    // Get step sizes
    int step_large, step_medium, step_small;
    if (zone) {
        if (brightening) {
            step_large = zone->step_sizes.large_up;
            step_medium = zone->step_sizes.medium_up;
            step_small = zone->step_sizes.small_up;
        } else {
            step_large = zone->step_sizes.large_down;
            step_medium = zone->step_sizes.medium_down;
            step_small = zone->step_sizes.small_down;
        }
    } else {
        if (brightening) {
            step_large = DEFAULT_STEP_LARGE;
            step_medium = DEFAULT_STEP_MEDIUM;
            step_small = DEFAULT_STEP_SMALL;
        } else {
            step_large = DEFAULT_STEP_LARGE / 2;
            step_medium = DEFAULT_STEP_MEDIUM / 2;
            step_small = DEFAULT_STEP_SMALL;
        }
    }

    // Determine step size and category
    if (info.error == 0) {
        info.step_size = 0;
        info.step_category = "none";
    } else if (abs_error > info.step_threshold_large) {
        info.step_size = step_large;
        info.step_category = brightening ? "large_up" : "large_down";
    } else if (abs_error > info.step_threshold_small) {
        info.step_size = step_medium;
        info.step_category = brightening ? "medium_up" : "medium_down";
    } else {
        info.step_size = step_small;
        info.step_category = brightening ? "small_up" : "small_down";
    }

    // Calculate next brightness
    if (std::abs(info.error) <= info.step_size) {
        info.next_brightness = target_brightness;
    } else {
        info.next_brightness = current_brightness + (info.error > 0 ? info.step_size : -info.step_size);
    }

    // Clamp to valid range
    info.next_brightness = std::max(0, std::min(100, info.next_brightness));

    return info;
}

int BrightnessController::getStepSize(int error, const Zone* zone) const {
    int abs_error = std::abs(error);
    bool brightening = (error > 0);

    // Get thresholds and step sizes from zone, or use defaults
    int threshold_large, threshold_small;
    int step_large, step_medium, step_small;

    if (zone) {
        threshold_large = zone->error_thresholds.large;
        threshold_small = zone->error_thresholds.small;

        // Use asymmetric step sizes based on direction
        // Brightening (entering bright areas): faster, safe for human vision
        // Dimming (entering dark areas): slower, safety-critical to prevent temporary blindness
        if (brightening) {
            step_large = zone->step_sizes.large_up;
            step_medium = zone->step_sizes.medium_up;
            step_small = zone->step_sizes.small_up;
        } else {
            step_large = zone->step_sizes.large_down;
            step_medium = zone->step_sizes.medium_down;
            step_small = zone->step_sizes.small_down;
        }
    } else {
        // Simple mode defaults - apply 2:1 asymmetry for safety
        threshold_large = DEFAULT_THRESHOLD_LARGE;
        threshold_small = DEFAULT_THRESHOLD_SMALL;
        if (brightening) {
            step_large = DEFAULT_STEP_LARGE;
            step_medium = DEFAULT_STEP_MEDIUM;
            step_small = DEFAULT_STEP_SMALL;
        } else {
            // Dimming is 50% slower for safety
            step_large = DEFAULT_STEP_LARGE / 2;
            step_medium = DEFAULT_STEP_MEDIUM / 2;
            step_small = DEFAULT_STEP_SMALL;  // Keep minimum at 1
        }
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
