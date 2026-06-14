#ifndef ALS_DIMMER_WP_ADJUST_RESTORE_HPP
#define ALS_DIMMER_WP_ADJUST_RESTORE_HPP

#include "als-dimmer/config.hpp"

#include <string>

namespace als_dimmer {

// Result of the wp_adjust boot restore - drives the legacy/new selection in
// main.cpp. A new-generation pixelpipe display exposes BOTH the legacy 0x1D
// slave AND the wp_adjust block on the new 0x1E slave; a legacy display exposes
// only 0x1D. So whether the wp_adjust ID answers on the new slave is the
// discriminator between the two white-point replay paths.
enum class WpAdjustRestoreStatus {
    NotPresent,  // disabled, or the wp_adjust ID did not answer on the new slave
                 // (legacy / non-pixelpipe display) -> caller runs the legacy
                 // wpx/wpy/wpz restore.
    Present,     // the wp_adjust block answered on the new slave (pixelpipe
                 // display). The new calibration was applied, or skipped because
                 // the file was missing/invalid (display left in pass-through) -
                 // either way the caller must NOT also run the legacy path.
};

// Boot-time restore of a wp_adjust (new-generation pixelpipe FPGA white-point
// block) calibration profile. Reads a wp-cal-v1 schema JSON (written by the
// disp-tester D65/match apps) and writes it to the FPGA over the new I2C
// slave's wp_adjust page, with readback verify and a frame-boundary COMMIT.
//
// Selection vs the legacy wpx/wpy/wpz replay (restoreWhitePointCalibration in
// main.cpp) is MUTUALLY EXCLUSIVE and decided at runtime by hardware probe:
//   1. cfg.enabled            (default false; only wp_adjust-capable display
//                              configs opt in - no I2C traffic otherwise)
//   2. wp_adjust ID probe on the configured new slave/page. If it does NOT
//      answer -> return NotPresent so the caller runs the legacy path
//      (only-0x1D legacy displays). If it answers -> this owns white-point for
//      the display; the caller skips the legacy path regardless of whether the
//      calibration file is present/valid.
//   3. (present) load + apply the wp-cal-v1 file, with readback verify + COMMIT.
// Every failure is fail-soft: log and continue; the display stays in
// pass-through and the daemon runs normally.
//
// Contract reference: fpga-wp-adjust docs/i2c-master-sw-integration-guideline.md
// (page transport: 2 bytes per logical register, big-endian,
// byte_addr = logical << 1; explicit {page, reg} pointer on every access).
//
// fallback_i2c_device is used when cfg.i2c_device is empty (normally the
// output's I2C bus, config.output.device).
WpAdjustRestoreStatus restoreWpAdjustCalibration(
    const WpAdjustCalibrationConfig& cfg,
    const std::string& fallback_i2c_device);

} // namespace als_dimmer

#endif // ALS_DIMMER_WP_ADJUST_RESTORE_HPP
