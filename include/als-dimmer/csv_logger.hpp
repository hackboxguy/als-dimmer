#ifndef ALS_DIMMER_CSV_LOGGER_HPP
#define ALS_DIMMER_CSV_LOGGER_HPP

#include <string>
#include <fstream>
#include <chrono>
#include <vector>

namespace als_dimmer {

/**
 * CSVLogger - Logs control loop data for analysis and visualization
 *
 * Records all critical control loop parameters in CSV format for
 * offline analysis, tuning, and debugging.
 */
class CSVLogger {
public:
    struct IterationData {
        // Timing
        double timestamp;           // Relative seconds since start
        uint64_t seq;              // Iteration counter

        // Sensor
        float lux;                 // Current lux reading
        bool sensor_healthy;       // Sensor health status

        // Zone
        std::string zone_name;     // Active zone (e.g., "indoor")
        bool zone_changed;         // Flag: zone transition occurred
        std::string curve;         // "linear" or "logarithmic"

        // Brightness
        int target_brightness;     // Target from curve mapping
        int current_brightness;    // Actual output brightness
        int previous_brightness;   // Previous iteration brightness
        int brightness_change;     // Delta from previous

        // Control
        int error;                 // target - current
        std::string step_category; // "large_up", "medium_down", etc.
        int step_size;             // Actual step value used
        int step_threshold_large;  // Zone's large threshold
        int step_threshold_small;  // Zone's small threshold

        // Mode
        std::string mode;          // "AUTO", "MANUAL", etc.

        // Manual override tracking (for ML analysis)
        bool manual_override_event;     // True if user manually adjusted brightness this iteration
        int auto_target_brightness;     // What AUTO mode would calculate (even in MANUAL mode)
        std::string override_type;      // "set_brightness", "adjust_brightness", or empty
        int hour_of_day;               // 0-23 for time-of-day patterns
        int day_of_week;               // 0-6 (0=Sunday) for weekly patterns
    };

    /**
     * Constructor - opens CSV file and writes header
     * @param file_path Path to CSV file (will be overwritten)
     */
    explicit CSVLogger(const std::string& file_path);

    /**
     * Destructor - flushes and closes file
     */
    ~CSVLogger();

    /**
     * Log one control loop iteration
     * @param data Iteration data to log
     */
    void logIteration(const IterationData& data);

    /**
     * Check if logger is operational
     * @return true if file is open and ready
     */
    bool isOpen() const { return file_.is_open(); }

private:
    std::ofstream file_;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<std::string> buffer_;  // Buffered rows
    size_t buffer_size_;
    std::chrono::steady_clock::time_point last_flush_;

    static constexpr size_t BUFFER_ROWS = 10;  // Flush every 10 rows
    static constexpr int FLUSH_INTERVAL_SEC = 5;  // Or every 5 seconds

    void writeHeader();
    void flush();
    std::string escapeCSV(const std::string& s) const;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_CSV_LOGGER_HPP
