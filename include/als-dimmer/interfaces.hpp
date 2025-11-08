#ifndef ALS_DIMMER_INTERFACES_HPP
#define ALS_DIMMER_INTERFACES_HPP

#include <string>

namespace als_dimmer {

/**
 * Abstract interface for ambient light sensors
 */
class SensorInterface {
public:
    virtual ~SensorInterface() = default;

    /**
     * Initialize sensor (open device, configure registers, etc.)
     * @return true on success, false on failure
     */
    virtual bool init() = 0;

    /**
     * Read current lux value
     * @return lux value on success, negative value on error
     */
    virtual float readLux() = 0;

    /**
     * Check if sensor is healthy (not timed out, responding correctly)
     * @return true if sensor is healthy
     */
    virtual bool isHealthy() const = 0;

    /**
     * Get sensor type name for logging
     * @return sensor type string
     */
    virtual std::string getType() const = 0;
};

/**
 * Abstract interface for brightness output devices
 */
class OutputInterface {
public:
    virtual ~OutputInterface() = default;

    /**
     * Initialize output (open device, detect display, etc.)
     * @return true on success, false on failure
     */
    virtual bool init() = 0;

    /**
     * Set brightness (0-100)
     * @param brightness Brightness value (0-100)
     * @return true on success, false on failure
     */
    virtual bool setBrightness(int brightness) = 0;

    /**
     * Get current brightness (0-100)
     * @return brightness on success, negative value on error
     */
    virtual int getCurrentBrightness() = 0;

    /**
     * Get output type name for logging
     * @return output type string
     */
    virtual std::string getType() const = 0;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_INTERFACES_HPP
