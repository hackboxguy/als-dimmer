#ifndef ALS_DIMMER_THERMAL_COMPENSATION_HPP
#define ALS_DIMMER_THERMAL_COMPENSATION_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace als_dimmer {

/**
 * ThermalCompensation - applies a temperature-dependent multiplicative
 * correction to the BrightnessToNitsLut output.
 *
 * The brightness-to-nits LUT captures the panel's response at the
 * temperature the sweep happened to be running at. Once the panel reaches
 * thermal equilibrium, LED junction efficiency and phosphor quantum yield
 * both drop, and the same brightness % produces 4-8% fewer nits. This
 * class loads a factor table (produced by tools/thermal-factor.py) that
 * captures the relationship of `nits / nits_at_reference_temp` vs panel
 * backlight temperature, polls a user-provided shell command to read the
 * current temperature on a low-frequency timer, and exposes a `factor()`
 * method that callers multiply against LUT-predicted nits to undo the
 * thermal drift.
 *
 * Failure modes are graceful by design:
 *   - Disabled / no table loaded               -> factor() returns 1.0
 *   - Polling not started                      -> factor() returns 1.0
 *   - First temp read hasn't completed yet     -> factor() returns 1.0
 *   - temp_command failed                      -> factor() returns last good
 *   - sustained temp_command failure (>5 min)  -> factor() returns 1.0,
 *                                                 has_reading() reports false
 *
 * In other words: when in doubt, the daemon falls back to "no correction",
 * which makes the system behave exactly as it did before the feature
 * existed. There is no failure mode that produces a bogus factor.
 */
class ThermalCompensation {
public:
    ThermalCompensation();
    ~ThermalCompensation();

    // Manages a thread - not safe to copy/move.
    ThermalCompensation(const ThermalCompensation&) = delete;
    ThermalCompensation& operator=(const ThermalCompensation&) = delete;

    /**
     * Parse the factor CSV at `path`. Returns true on success.
     * Format (produced by tools/thermal-factor.py):
     *   # comments
     *   # reference_temp_c=38.0
     *   # label=...
     *   backlight_temp_c,factor
     *   38.0,1.0000
     *   ...
     */
    bool loadFactorTable(const std::string& path);

    /**
     * Spawn the temperature polling thread. `temp_command` is a shell command
     * that prints the current backlight temperature (in degC) on stdout; the
     * first signed/decimal float in stdout is parsed. `poll_interval_sec`
     * controls how often the command is run; lower = more responsive
     * compensation, higher = lower I2C/CPU load. Safe to call only once.
     */
    void startPolling(const std::string& temp_command, int poll_interval_sec);

    /**
     * Signal the polling thread to exit and join it. Idempotent. Called
     * automatically by the destructor; explicit calls let the caller control
     * shutdown timing during a clean daemon-exit sequence.
     */
    void stopPolling();

    /**
     * True when a factor table is loaded AND polling has been started.
     * Used by the daemon to decide whether to surface thermal fields in
     * JSON status responses. When false, factor() always returns 1.0.
     */
    bool isEnabled() const { return table_loaded_ && polling_started_; }

    /**
     * The multiplicative correction to apply to LUT-predicted nits, based
     * on the most recent successful temperature reading. Returns 1.0 when:
     *   - thermal compensation is disabled, OR
     *   - no successful temperature reading yet, OR
     *   - last successful reading is older than the staleness watchdog
     *     (currently hardcoded to 5x poll_interval_sec, capped at 5 min).
     * Otherwise interpolates the factor at the cached temperature, clamping
     * to the table's endpoints when temp is outside the measured range.
     *
     * Thread-safe. Called from the main daemon loop on every command.
     */
    double factor() const;

    /**
     * True if at least one successful temperature reading has happened
     * AND it isn't stale. Used for diagnostics - distinct from isEnabled()
     * which only reports configuration state.
     */
    bool hasReading() const;

    /**
     * Most recent successful temperature reading in degC. NaN if none.
     * Thread-safe.
     */
    double lastTempC() const;

    /**
     * Milliseconds since the most recent successful temperature reading.
     * -1 if no reading has succeeded yet. Thread-safe.
     */
    int64_t lastReadAgeMs() const;

    // Metadata from the loaded factor table (constant after loadFactorTable).
    const std::string& label() const           { return label_; }
    const std::string& sourcePath() const      { return source_path_; }
    double referenceTempC() const              { return reference_temp_c_; }
    double minFactor() const                   { return min_factor_; }
    double maxFactor() const                   { return max_factor_; }
    size_t rowCount() const                    { return temps_.size(); }

private:
    void pollLoop();
    void runOneTempCheck();
    static double parseTempFromOutput(const std::string& output);

    // Interpolate a factor at the given temp (no locking - caller manages).
    double interpolateFactorAt(double temp_c) const;

    // Factor table (sorted ascending by temperature, parallel arrays)
    std::vector<double> temps_;
    std::vector<double> factors_;
    double reference_temp_c_ = 0.0;
    double min_factor_ = 1.0;
    double max_factor_ = 1.0;
    std::string label_;
    std::string source_path_;
    bool table_loaded_ = false;

    // Polling thread state
    std::string temp_command_;
    int poll_interval_sec_ = 30;
    std::thread poll_thread_;
    std::atomic<bool> stop_requested_{false};
    bool polling_started_ = false;

    // Cached reading state - protected by mu_
    mutable std::mutex mu_;
    double last_temp_c_ = 0.0;          // valid only when has_reading_ == true
    std::chrono::steady_clock::time_point last_read_time_;
    bool has_reading_ = false;

    // Failure tracking for log throttling (protected by mu_)
    int consecutive_failures_ = 0;
    bool warned_about_sustained_failure_ = false;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_THERMAL_COMPENSATION_HPP
