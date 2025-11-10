#ifndef ALS_DIMMER_ZONE_MAPPER_HPP
#define ALS_DIMMER_ZONE_MAPPER_HPP

#include "config.hpp"
#include <vector>
#include <string>

namespace als_dimmer {

/**
 * ZoneMapper handles multi-zone brightness mapping with different curves
 *
 * Supports:
 * - Multiple lux zones with independent brightness ranges
 * - Linear and logarithmic curve types
 * - Automatic zone selection based on current lux
 * - Optional hysteresis to prevent oscillation at zone boundaries
 */
class ZoneMapper {
public:
    explicit ZoneMapper(const std::vector<Zone>& zones, float hysteresis_percent = 0.0f);

    // Map lux value to brightness (0-100) using appropriate zone and curve
    int mapLuxToBrightness(float lux) const;

    // Get the current active zone for a given lux value
    // Uses hysteresis if enabled to prevent zone oscillation
    const Zone* selectZone(float lux) const;

    // Get zone name for logging/debugging
    std::string getCurrentZoneName(float lux) const;

private:
    // Curve calculation functions
    int calculateLinear(float lux, const Zone& zone) const;
    int calculateLogarithmic(float lux, const Zone& zone) const;

    std::vector<Zone> zones_;
    mutable const Zone* current_zone_ = nullptr;  // Track current zone for hysteresis
    float hysteresis_percent_;                     // Hysteresis percentage (0 = disabled)
};

} // namespace als_dimmer

#endif // ALS_DIMMER_ZONE_MAPPER_HPP
