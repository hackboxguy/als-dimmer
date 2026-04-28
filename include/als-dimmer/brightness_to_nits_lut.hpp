#ifndef ALS_DIMMER_BRIGHTNESS_TO_NITS_LUT_HPP
#define ALS_DIMMER_BRIGHTNESS_TO_NITS_LUT_HPP

#include <string>
#include <vector>

namespace als_dimmer {

/**
 * BrightnessToNitsLut - Loads a sweep table mapping brightness % (0..100)
 * to absolute luminance in nits, and provides forward / reverse interpolation.
 *
 * The LUT is independent of which output type is in use - it characterizes
 * the entire stack (output device + display panel + thermal state) from the
 * caller's perspective. Used by:
 *   - get_absolute_brightness  (forward: % -> nits)
 *   - set_absolute_brightness  (reverse: nits -> %)
 *   - get_status               (forward: report nits alongside brightness %)
 *
 * CSV format (produced by tools/als-dimmer-sweep.py):
 *   # comments, including: label, output_type, timestamp, temp_source
 *   brightness_pct,nits,status,backlight_temp_c
 *   100,1119.80,OK,42.5
 *   99,1110.40,OK,42.7
 *   ...
 *   0,0.05,OK,
 *
 * Only rows with status == "OK" are loaded. backlight_temp_c is recorded
 * but currently unused (reserved for future thermal compensation).
 *
 * Out-of-range queries clamp to the loaded endpoints and report it via
 * the `clamped` reference parameter.
 */
class BrightnessToNitsLut {
public:
    BrightnessToNitsLut() = default;

    // Parses the CSV file. Returns true on success, false on any error
    // (file missing, malformed, no OK rows, etc.). Errors logged via LOG_*.
    // is_loaded() reflects the final state.
    bool loadFromFile(const std::string& path);

    bool is_loaded() const { return loaded_; }

    // Forward: brightness % -> nits. Linear interpolation between rows.
    // If brightness is outside the LUT's [min_pct, max_pct] range, clamps
    // to the nearest endpoint and sets clamped=true.
    // Caller must check is_loaded() first.
    double pctToNits(double brightness_pct, bool& clamped) const;

    // Reverse: nits -> brightness %. Linear interpolation. Clamps + flag
    // when target is outside [min_nits, max_nits].
    // Caller must check is_loaded() first.
    double nitsToPct(double target_nits, bool& clamped) const;

    double min_nits() const { return min_nits_; }
    double max_nits() const { return max_nits_; }
    double min_pct() const  { return min_pct_;  }
    double max_pct() const  { return max_pct_;  }

    // Metadata extracted from CSV "# key=value" comment lines. Empty when
    // not present. The daemon checks `output_type` against config.output.type
    // at load time and warns on mismatch.
    const std::string& output_type_tag() const { return output_type_tag_; }
    const std::string& label() const           { return label_; }
    const std::string& source_path() const     { return source_path_; }
    size_t row_count() const                   { return pct_.size(); }

private:
    // Sorted ascending by brightness_pct.
    std::vector<double> pct_;
    std::vector<double> nits_;

    // Same data sorted ascending by nits for the reverse lookup.
    std::vector<double> nits_sorted_;
    std::vector<double> pct_for_nits_sorted_;

    double min_pct_   = 0.0;
    double max_pct_   = 0.0;
    double min_nits_  = 0.0;
    double max_nits_  = 0.0;

    std::string output_type_tag_;
    std::string label_;
    std::string source_path_;

    bool loaded_ = false;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_BRIGHTNESS_TO_NITS_LUT_HPP
