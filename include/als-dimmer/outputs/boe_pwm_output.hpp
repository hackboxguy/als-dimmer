#ifndef ALS_DIMMER_BOE_PWM_OUTPUT_HPP
#define ALS_DIMMER_BOE_PWM_OUTPUT_HPP

#include "als-dimmer/interfaces.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace als_dimmer {

/**
 * BoePwmOutput - Backlight control for BOE displays driven by an MPS MPQ3367
 * LED driver, dimmed via a Pi PWM pin.
 *
 * On init():
 *   1. Configures the MPQ3367 over I2C (4 channels, pure PWM, phase-shift on)
 *      and clears the FT_LEDO power-on latch by double-reading register 0x02.
 *      Skipped if skip_chip_config is true.
 *   2. Forces the PWM GPIO into its alt function via `pinctrl` (warns and
 *      continues on failure - dtoverlay in /boot/firmware/config.txt is the
 *      authoritative source for pin mux).
 *   3. Exports the PWM channel under /sys/class/pwm/<chip>/, sets the period
 *      and enables it.
 *   4. If response_curve_path is non-empty, parses the CSV and switches
 *      setBrightness from direct duty mapping to linear-in-nits via inverse
 *      interpolation through the curve.
 *
 * On setBrightness(0..100):
 *   - No curve: duty_pct = brightness.
 *   - Curve   : target_nits = brightness/100 * curve_max_nits, then bisect
 *               interpolate to duty_pct.
 *   The duty_ns write uses period-1 at 100% to avoid the BCM2711 quirk and
 *   0 ns at 0%.
 */
class BoePwmOutput : public OutputInterface {
public:
    BoePwmOutput(const std::string& i2c_device,
                 uint8_t i2c_address,
                 const std::string& pwm_chip,
                 int pwm_channel,
                 int pwm_gpio,
                 const std::string& pwm_alt_func,
                 int period_ns,
                 const std::string& response_curve_path,
                 bool skip_chip_config);
    ~BoePwmOutput() override;

    bool init() override;
    bool setBrightness(int brightness) override;
    int getCurrentBrightness() override;
    std::string getType() const override;

private:
    bool configureMpq3367();
    void assertPinMux();
    bool setupPwm();
    bool loadResponseCurve();
    bool writeSysfs(const std::string& path, const std::string& value);

    int dutyPctToNs(double duty_pct) const;
    double brightnessToDutyPct(int brightness) const;

    // Configuration
    std::string i2c_device_;
    uint8_t i2c_address_;
    std::string pwm_chip_;
    int pwm_channel_;
    int pwm_gpio_;
    std::string pwm_alt_func_;
    int period_ns_;
    std::string response_curve_path_;
    bool skip_chip_config_;

    // Resolved sysfs paths
    std::string pwm_chip_dir_;
    std::string pwm_channel_dir_;
    std::string duty_cycle_path_;

    // Runtime state
    int i2c_fd_;
    int current_brightness_;
    int last_duty_ns_;

    // Optional response curve (sorted ascending by nits)
    std::vector<double> curve_nits_;
    std::vector<double> curve_duty_pct_;
    double curve_max_nits_;
    bool curve_loaded_;
};

std::unique_ptr<OutputInterface> createBoePwmOutput(const std::string& i2c_device,
                                                    uint8_t i2c_address,
                                                    const std::string& pwm_chip,
                                                    int pwm_channel,
                                                    int pwm_gpio,
                                                    const std::string& pwm_alt_func,
                                                    int period_ns,
                                                    const std::string& response_curve_path,
                                                    bool skip_chip_config);

} // namespace als_dimmer

#endif // ALS_DIMMER_BOE_PWM_OUTPUT_HPP
