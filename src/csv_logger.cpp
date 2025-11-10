#include "als-dimmer/csv_logger.hpp"
#include "als-dimmer/logger.hpp"
#include <sstream>
#include <iomanip>

namespace als_dimmer {

CSVLogger::CSVLogger(const std::string& file_path)
    : start_time_(std::chrono::steady_clock::now())
    , buffer_size_(0)
    , last_flush_(std::chrono::steady_clock::now()) {

    file_.open(file_path, std::ios::out | std::ios::trunc);

    if (!file_.is_open()) {
        LOG_ERROR("CSVLogger", "Failed to open CSV file: " << file_path);
        return;
    }

    LOG_INFO("CSVLogger", "Opened CSV log file: " << file_path);
    writeHeader();
    buffer_.reserve(BUFFER_ROWS);
}

CSVLogger::~CSVLogger() {
    if (file_.is_open()) {
        flush();
        file_.close();
        LOG_INFO("CSVLogger", "Closed CSV log file");
    }
}

void CSVLogger::writeHeader() {
    file_ << "timestamp,seq,lux,zone,zone_changed,curve,"
          << "target_brightness,current_brightness,previous_brightness,"
          << "error,step_category,step_size,"
          << "step_threshold_large,step_threshold_small,"
          << "brightness_change,mode,sensor_healthy\n";
    file_.flush();  // Ensure header is written immediately
}

void CSVLogger::logIteration(const IterationData& data) {
    if (!file_.is_open()) {
        return;
    }

    // Build CSV row
    std::ostringstream row;
    row << std::fixed << std::setprecision(3) << data.timestamp << ","
        << data.seq << ","
        << std::setprecision(1) << data.lux << ","
        << escapeCSV(data.zone_name) << ","
        << (data.zone_changed ? "1" : "0") << ","
        << escapeCSV(data.curve) << ","
        << data.target_brightness << ","
        << data.current_brightness << ","
        << data.previous_brightness << ","
        << data.error << ","
        << escapeCSV(data.step_category) << ","
        << data.step_size << ","
        << data.step_threshold_large << ","
        << data.step_threshold_small << ","
        << data.brightness_change << ","
        << escapeCSV(data.mode) << ","
        << (data.sensor_healthy ? "1" : "0") << "\n";

    // Add to buffer
    buffer_.push_back(row.str());
    buffer_size_++;

    // Flush if buffer is full or time elapsed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_flush_).count();

    if (buffer_size_ >= BUFFER_ROWS || elapsed >= FLUSH_INTERVAL_SEC) {
        flush();
    }
}

void CSVLogger::flush() {
    if (buffer_.empty() || !file_.is_open()) {
        return;
    }

    // Write all buffered rows
    for (const auto& row : buffer_) {
        file_ << row;
    }

    file_.flush();
    buffer_.clear();
    buffer_size_ = 0;
    last_flush_ = std::chrono::steady_clock::now();

    LOG_TRACE("CSVLogger", "Flushed " << buffer_size_ << " rows to CSV");
}

std::string CSVLogger::escapeCSV(const std::string& s) const {
    // Simple CSV escaping: quote strings with commas/quotes
    if (s.find(',') != std::string::npos || s.find('"') != std::string::npos) {
        std::string escaped = "\"";
        for (char c : s) {
            if (c == '"') {
                escaped += "\"\"";  // Escape quotes by doubling
            } else {
                escaped += c;
            }
        }
        escaped += "\"";
        return escaped;
    }
    return s;
}

} // namespace als_dimmer
