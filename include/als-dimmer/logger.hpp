#ifndef ALS_DIMMER_LOGGER_HPP
#define ALS_DIMMER_LOGGER_HPP

#include <iostream>
#include <sstream>
#include <string>
#include <ctime>
#include <iomanip>
#include <mutex>

namespace als_dimmer {

/**
 * Log levels in order of severity
 */
enum class LogLevel {
    TRACE = 0,  // Very verbose, step-by-step execution
    DEBUG = 1,  // Detailed diagnostic information
    INFO  = 2,  // Important operational messages (default)
    WARN  = 3,  // Warning conditions
    ERROR = 4   // Critical errors
};

/**
 * Simple thread-safe logger
 */
class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLevel(LogLevel level) {
        current_level_ = level;
    }

    LogLevel getLevel() const {
        return current_level_;
    }

    bool shouldLog(LogLevel level) const {
        return level >= current_level_;
    }

    void log(LogLevel level, const std::string& component, const std::string& message) {
        if (!shouldLog(level)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // Get timestamp
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);

        // Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] [Component] Message
        std::cout << "["
                  << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
                  << "] ["
                  << levelToString(level)
                  << "] ["
                  << component
                  << "] "
                  << message
                  << "\n";
        std::cout.flush();
    }

    static std::string levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE: return "TRACE";
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            default: return "?????";
        }
    }

    static LogLevel stringToLevel(const std::string& level_str) {
        std::string lower = level_str;
        for (auto& c : lower) c = std::tolower(c);

        if (lower == "trace") return LogLevel::TRACE;
        if (lower == "debug") return LogLevel::DEBUG;
        if (lower == "info")  return LogLevel::INFO;
        if (lower == "warn" || lower == "warning") return LogLevel::WARN;
        if (lower == "error") return LogLevel::ERROR;

        return LogLevel::INFO; // Default
    }

private:
    Logger() : current_level_(LogLevel::INFO) {}
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel current_level_;
    std::mutex mutex_;
};

} // namespace als_dimmer

// Convenience macros for logging
#define LOG_TRACE(component, ...) \
    do { \
        if (als_dimmer::Logger::getInstance().shouldLog(als_dimmer::LogLevel::TRACE)) { \
            std::ostringstream oss; \
            oss << __VA_ARGS__; \
            als_dimmer::Logger::getInstance().log(als_dimmer::LogLevel::TRACE, component, oss.str()); \
        } \
    } while(0)

#define LOG_DEBUG(component, ...) \
    do { \
        if (als_dimmer::Logger::getInstance().shouldLog(als_dimmer::LogLevel::DEBUG)) { \
            std::ostringstream oss; \
            oss << __VA_ARGS__; \
            als_dimmer::Logger::getInstance().log(als_dimmer::LogLevel::DEBUG, component, oss.str()); \
        } \
    } while(0)

#define LOG_INFO(component, ...) \
    do { \
        if (als_dimmer::Logger::getInstance().shouldLog(als_dimmer::LogLevel::INFO)) { \
            std::ostringstream oss; \
            oss << __VA_ARGS__; \
            als_dimmer::Logger::getInstance().log(als_dimmer::LogLevel::INFO, component, oss.str()); \
        } \
    } while(0)

#define LOG_WARN(component, ...) \
    do { \
        if (als_dimmer::Logger::getInstance().shouldLog(als_dimmer::LogLevel::WARN)) { \
            std::ostringstream oss; \
            oss << __VA_ARGS__; \
            als_dimmer::Logger::getInstance().log(als_dimmer::LogLevel::WARN, component, oss.str()); \
        } \
    } while(0)

#define LOG_ERROR(component, ...) \
    do { \
        std::ostringstream oss; \
        oss << __VA_ARGS__; \
        als_dimmer::Logger::getInstance().log(als_dimmer::LogLevel::ERROR, component, oss.str()); \
    } while(0)

#endif // ALS_DIMMER_LOGGER_HPP
