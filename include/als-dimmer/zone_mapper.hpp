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
 */
class ZoneMapper {
public:
    explicit ZoneMapper(const std::vector<Zone>& zones);

    // Map lux value to brightness (0-100) using appropriate zone and curve
    int mapLuxToBrightness(float lux) const;

    // Get the current active zone for a given lux value
    const Zone* selectZone(float lux) const;

    // Get zone name for logging/debugging
    std::string getCurrentZoneName(float lux) const;

private:
    // Curve calculation functions
    int calculateLinear(float lux, const Zone& zone) const;
    int calculateLogarithmic(float lux, const Zone& zone) const;

    std::vector<Zone> zones_;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_ZONE_MAPPER_HPP
