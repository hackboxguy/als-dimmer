#include "als-dimmer/zone_mapper.hpp"
#include "als-dimmer/logger.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace als_dimmer {

ZoneMapper::ZoneMapper(const std::vector<Zone>& zones)
    : zones_(zones) {
    if (zones_.empty()) {
        throw std::runtime_error("ZoneMapper: At least one zone is required");
    }
}

const Zone* ZoneMapper::selectZone(float lux) const {
    // Find the zone that contains this lux value
    for (const auto& zone : zones_) {
        float lux_min = zone.lux_range[0];
        float lux_max = zone.lux_range[1];

        if (lux >= lux_min && lux < lux_max) {
            return &zone;
        }
    }

    // If lux is beyond all zones, use the last zone
    return &zones_.back();
}

std::string ZoneMapper::getCurrentZoneName(float lux) const {
    const Zone* zone = selectZone(lux);
    return zone ? zone->name : "unknown";
}

int ZoneMapper::calculateLinear(float lux, const Zone& zone) const {
    float lux_min = zone.lux_range[0];
    float lux_max = zone.lux_range[1];
    int bright_min = zone.brightness_range[0];
    int bright_max = zone.brightness_range[1];

    // Clamp lux to zone range
    float lux_clamped = std::max(lux_min, std::min(lux, lux_max));

    // Linear interpolation
    // brightness = bright_min + (lux - lux_min) / (lux_max - lux_min) * (bright_max - bright_min)
    float lux_range = lux_max - lux_min;
    if (lux_range <= 0.0f) {
        return bright_min;  // Avoid division by zero
    }

    float normalized = (lux_clamped - lux_min) / lux_range;
    int brightness = bright_min + static_cast<int>(normalized * (bright_max - bright_min));

    // Clamp to valid brightness range
    return std::max(0, std::min(100, brightness));
}

int ZoneMapper::calculateLogarithmic(float lux, const Zone& zone) const {
    float lux_min = zone.lux_range[0];
    float lux_max = zone.lux_range[1];
    int bright_min = zone.brightness_range[0];
    int bright_max = zone.brightness_range[1];

    // Clamp lux to zone range
    float lux_clamped = std::max(lux_min, std::min(lux, lux_max));

    // Logarithmic mapping
    // We use log(1 + x) to avoid log(0) issues
    // normalized = log(1 + (lux - lux_min)) / log(1 + (lux_max - lux_min))
    float lux_offset = lux_clamped - lux_min;
    float lux_range = lux_max - lux_min;

    if (lux_range <= 0.0f) {
        return bright_min;  // Avoid division by zero
    }

    // Add 1 to avoid log(0), then normalize
    float normalized = std::log(1.0f + lux_offset) / std::log(1.0f + lux_range);
    int brightness = bright_min + static_cast<int>(normalized * (bright_max - bright_min));

    // Clamp to valid brightness range
    return std::max(0, std::min(100, brightness));
}

int ZoneMapper::mapLuxToBrightness(float lux) const {
    // Handle invalid lux values
    if (lux < 0.0f) {
        std::cerr << "[ZoneMapper] Warning: Negative lux value (" << lux << "), using zone minimum\n";
        lux = 0.0f;
    }

    // Select the appropriate zone
    const Zone* zone = selectZone(lux);
    if (!zone) {
        LOG_ERROR("ZoneMapper", "No zone found for lux=" << lux);
        return 50;  // Fallback to 50%
    }

    // Calculate brightness using the zone's curve type
    int brightness;
    if (zone->curve == "logarithmic") {
        brightness = calculateLogarithmic(lux, *zone);
    } else {
        // Default to linear (also handles "linear" explicitly)
        brightness = calculateLinear(lux, *zone);
    }

    // Debug output for first few calls
    static int debug_count = 0;
    if (debug_count++ < 5) {
        LOG_DEBUG("ZoneMapper", "Lux=" << lux
                  << " Zone=" << zone->name
                  << " Curve=" << zone->curve
                  << " Brightness=" << brightness << "%");
    }

    return brightness;
}

} // namespace als_dimmer
