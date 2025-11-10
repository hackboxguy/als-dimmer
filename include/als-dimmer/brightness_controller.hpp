#ifndef ALS_DIMMER_BRIGHTNESS_CONTROLLER_HPP
#define ALS_DIMMER_BRIGHTNESS_CONTROLLER_HPP

#include "config.hpp"
#include <string>

namespace als_dimmer {

/**
 * BrightnessController manages smooth brightness transitions
 *
 * Uses error-based step sizing for responsive yet smooth adjustments:
 * - Large errors: Fast convergence (large steps)
 * - Medium errors: Moderate convergence (medium steps)
 * - Small errors: Fine-tuning (small steps)
 *
 * This prevents jarring brightness changes while maintaining responsiveness.
 */
class BrightnessController {
public:
    /**
     * Diagnostic information about a brightness transition
     */
    struct TransitionInfo {
        int error;                  // target - current
        int step_size;              // step value used
        std::string step_category;  // "large_up", "medium_down", "small_up", etc.
        int step_threshold_large;   // large error threshold
        int step_threshold_small;   // small error threshold
        int next_brightness;        // calculated next value
    };

    BrightnessController();

    /**
     * Calculate the next brightness value with smooth ramping
     *
     * @param target_brightness Desired brightness (0-100)
     * @param current_brightness Current brightness (0-100)
     * @param zone Current zone (for step sizes and thresholds), nullptr for simple mode
     * @return Next brightness value to apply
     */
    int calculateNextBrightness(int target_brightness,
                                int current_brightness,
                                const Zone* zone) const;

    /**
     * Calculate next brightness with diagnostic information
     *
     * @param target_brightness Desired brightness (0-100)
     * @param current_brightness Current brightness (0-100)
     * @param zone Current zone (for step sizes and thresholds), nullptr for simple mode
     * @return Transition info with next brightness and diagnostics
     */
    TransitionInfo calculateNextBrightnessWithInfo(int target_brightness,
                                                    int current_brightness,
                                                    const Zone* zone) const;

private:
    /**
     * Get step size based on error magnitude
     *
     * @param error Brightness error (target - current)
     * @param zone Current zone (for thresholds and step sizes)
     * @return Step size to use
     */
    int getStepSize(int error, const Zone* zone) const;

    // Default step sizes for simple mode (no zones)
    static constexpr int DEFAULT_STEP_LARGE = 5;
    static constexpr int DEFAULT_STEP_MEDIUM = 2;
    static constexpr int DEFAULT_STEP_SMALL = 1;

    // Default error thresholds for simple mode
    static constexpr int DEFAULT_THRESHOLD_LARGE = 20;
    static constexpr int DEFAULT_THRESHOLD_SMALL = 5;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_BRIGHTNESS_CONTROLLER_HPP
