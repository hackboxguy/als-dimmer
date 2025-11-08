#ifndef ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP
#define ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP

#include "als-dimmer/interfaces.hpp"
#include <string>
#include <cstdint>

namespace als_dimmer {

/**
 * I2CDimmerOutput - Generic I2C dimmer output for custom displays
 *
 * Supports two dimmer types:
 * - DIMMER_200: Brightness range 0-200 (command 0x28, 1-byte value)
 * - DIMMER_800: Brightness range 0-800 (command 0x35, 2-byte big-endian value)
 *
 * I2C command format:
 * - Common header: 0x00 0x00 0x00
 * - Command byte: 0x28 (dimmer200) or 0x35 (dimmer800)
 * - Value bytes: 1 byte (dimmer200) or 2 bytes BE (dimmer800)
 */
class I2CDimmerOutput : public OutputInterface {
public:
    enum class DimmerType {
        DIMMER_200,  // 0-200 range, command 0x28
        DIMMER_800   // 0-800 range, command 0x35
    };

    /**
     * Construct I2C dimmer output
     *
     * @param device I2C device path (e.g., "/dev/i2c-1")
     * @param address I2C slave address (e.g., 0x1D)
     * @param type Dimmer type (DIMMER_200 or DIMMER_800)
     */
    I2CDimmerOutput(const std::string& device, uint8_t address, DimmerType type);
    ~I2CDimmerOutput();

    bool init() override;
    bool setBrightness(int brightness) override;
    int getCurrentBrightness() override;
    std::string getType() const override;

private:
    std::string device_;
    uint8_t address_;
    DimmerType type_;
    int fd_;
    int current_brightness_;  // Cached brightness (0-100)
    int max_native_brightness_;  // 200 or 800
    uint8_t command_byte_;  // 0x28 or 0x35

    /**
     * Write brightness value to I2C dimmer
     *
     * @param native_value Brightness in native range (0-200 or 0-800)
     * @return true on success
     */
    bool writeI2CBrightness(int native_value);

    /**
     * Scale percentage (0-100) to native brightness value
     *
     * @param percent Brightness percentage (0-100)
     * @return Native brightness value (0-200 or 0-800)
     */
    int scaleToNative(int percent) const;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP
