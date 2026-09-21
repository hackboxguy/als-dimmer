#ifndef ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP
#define ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP

#include "als-dimmer/interfaces.hpp"
#include <string>
#include <cstdint>

namespace als_dimmer {

/**
 * I2CDimmerOutput - Generic I2C dimmer output for custom displays
 *
 * Supports three dimmer types:
 * - DIMMER_200: Brightness range 0-200 (command 0x28, 1-byte value)
 * - DIMMER_800: Brightness range 0-800 (command 0x35, 2-byte BCD-encoded value)
 * - DIMMER_2048: Brightness range 0-2048 (command 0x35, 2-byte binary big-endian value)
 *
 * I2C command format:
 * - Common header: 0x00 0x00 0x00
 * - Command byte: 0x28 (dimmer200) or 0x35 (dimmer800/dimmer2048)
 * - Value bytes: 1 byte (dimmer200) or 2 bytes BE (dimmer800 BCD / dimmer2048 binary)
 *
 * DIMMER_800 and DIMMER_2048 are the same register reached two ways, and at
 * 100% they put the SAME BYTES on the wire: BCD(800) and binary 2048 are both
 * 0x08 0x00.  They diverge everywhere else -- at 10%, BCD(80) = 0x0080 = 128
 * against binary 204 -- so which one is correct depends on what the FPGA
 * decodes, and a measurement taken only at 100% cannot tell them apart.
 * Measured on 12.3"-NQ1.1 (Lattice-25) 2026-09-21: it responds linearly to
 * arbitrary binary values including 0x09C4 and 0x0C8F, whose low bytes are not
 * valid BCD, so that board decodes BINARY and DIMMER_2048 is the right type
 * for it.
 *
 * The native maximum is overridable per config via output.value_range[1],
 * because the register is a raw PWM duty whose full scale is a property of the
 * bitstream rather than of this driver.  See the constructor.
 */
class I2CDimmerOutput : public OutputInterface {
public:
    enum class DimmerType {
        DIMMER_200,  // 0-200 range, command 0x28
        DIMMER_800,  // 0-800 range, command 0x35, BCD encoding
        DIMMER_2048  // 0-2048 range, command 0x35, binary encoding
    };

    /**
     * Construct I2C dimmer output
     *
     * @param device I2C device path (e.g., "/dev/i2c-1")
     * @param address I2C slave address (e.g., 0x1D)
     * @param type Dimmer type (DIMMER_200, DIMMER_800, or DIMMER_2048)
     * @param max_native Native full-scale value, or 0 to use the type's
     *        default (200 / 800 / 2048).  Comes from output.value_range[1].
     *        Rejected for DIMMER_200 and DIMMER_800 above their encodings'
     *        limits -- see the constructor.
     */
    I2CDimmerOutput(const std::string& device, uint8_t address, DimmerType type,
                    int max_native = 0);
    ~I2CDimmerOutput();

    bool init() override;
    bool setBrightness(int brightness) override;
    int getCurrentBrightness() override;
    std::string getType() const override;
    bool setWhitePoint(int wpx, int wpy, int wpz) override;

private:
    static constexpr uint8_t REG_WP_X = 0x37;
    static constexpr uint8_t REG_WP_Y = 0x39;
    static constexpr uint8_t REG_WP_Z = 0x3B;

    std::string device_;
    uint8_t address_;
    DimmerType type_;
    int fd_;
    int current_brightness_;  // Cached brightness (0-100)
    int max_native_brightness_;  // 200, 800, 2048, or an value_range[1] override
    uint8_t command_byte_;  // 0x28 or 0x35

    /**
     * Write brightness value to I2C dimmer
     *
     * @param native_value Brightness in native range (0-200, 0-800, or 0-2048)
     * @return true on success
     */
    bool writeI2CBrightness(int native_value);

    /**
     * Write one FPGA white-point register as a 16-bit big-endian value.
     *
     * @param reg FPGA register offset (0x37, 0x39, or 0x3B)
     * @param value White-point value in [0, 256]
     * @return true on success
     */
    bool writeWhitePointRegister(uint8_t reg, int value);

    /**
     * Scale percentage (0-100) to native brightness value
     *
     * @param percent Brightness percentage (0-100)
     * @return Native brightness value (0-200, 0-800, or 0-2048)
     */
    int scaleToNative(int percent) const;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_I2C_DIMMER_OUTPUT_HPP
