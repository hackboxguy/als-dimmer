#ifndef ALS_DIMMER_NULL_SENSOR_HPP
#define ALS_DIMMER_NULL_SENSOR_HPP

#include "als-dimmer/interfaces.hpp"

namespace als_dimmer {

/**
 * NullSensor - Stand-in sensor used when no real ALS is reachable.
 *
 * Used in two scenarios:
 *   1. Sensor init fails at startup (e.g. no hardware wired): the factory
 *      installs a NullSensor so the daemon can still run in MANUAL mode.
 *   2. A live sensor goes unhealthy for longer than the watchdog timeout:
 *      main loop swaps the live sensor pointer for a NullSensor and forces
 *      MANUAL mode, keeping the rest of the code path identical to (1).
 *
 * readLux() always returns -1.0f, which the main loop already treats as
 * "no usable lux this tick" and skips the AUTO update. isHealthy() is
 * always false so any code checking sensor health will see the NullSensor
 * as unavailable.
 */
class NullSensor : public SensorInterface {
public:
    bool init() override { return true; }
    float readLux() override { return -1.0f; }
    bool isHealthy() const override { return false; }
    std::string getType() const override { return "null"; }
};

} // namespace als_dimmer

#endif // ALS_DIMMER_NULL_SENSOR_HPP
