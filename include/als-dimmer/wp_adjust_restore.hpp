#ifndef ALS_DIMMER_WP_ADJUST_RESTORE_HPP
#define ALS_DIMMER_WP_ADJUST_RESTORE_HPP

#include "als-dimmer/config.hpp"

#include <string>

namespace als_dimmer {

// Boot-time restore of a wp_adjust (new-generation pixelpipe FPGA white-point
// block) calibration profile. Reads a wp-cal-v1 schema JSON (written by the
// disp-tester D65/match apps) and writes it to the FPGA over the new I2C
// slave's wp_adjust page, with readback verify and a frame-boundary COMMIT.
//
// This path is ADDITIVE to and fully independent of the legacy wpx/wpy/wpz
// replay (restoreWhitePointCalibration in main.cpp), which remains
// unconditional for the Lattice legacy displays. Gating, in order:
//   1. cfg.enabled            (default false; only wp_adjust-capable display
//                              configs opt in - no I2C traffic otherwise)
//   2. calibration file exists
//   3. wp_adjust ID/VERSION/FRAC_BITS probe answers on the configured
//      address/page (skip quietly otherwise)
// Every failure is fail-soft: log and return false; the display stays in
// pass-through and the daemon continues normally.
//
// Contract reference: fpga-wp-adjust docs/i2c-master-sw-integration-guideline.md
// (page transport: 2 bytes per logical register, big-endian,
// byte_addr = logical << 1; explicit {page, reg} pointer on every access).
//
// fallback_i2c_device is used when cfg.i2c_device is empty (normally the
// output's I2C bus, config.output.device).
bool restoreWpAdjustCalibration(const WpAdjustCalibrationConfig& cfg,
                                const std::string& fallback_i2c_device);

} // namespace als_dimmer

#endif // ALS_DIMMER_WP_ADJUST_RESTORE_HPP
