#include "als-dimmer/thermal_compensation.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <regex>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace als_dimmer {

namespace {

// Trim ASCII whitespace from both ends. Tolerates the trailing \r that some
// CSVs ship with (Windows line endings, copy-pasted content).
void trim(std::string& s) {
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && is_ws(s[start])) ++start;
    if (start > 0) s.erase(0, start);
}

// Parse "# key=value" comment lines and extract metadata. Mirrors the same
// pattern used by BrightnessToNitsLut so users get consistent behavior
// across the two CSV types.
void parseMetadataLine(const std::string& line,
                       double& reference_temp_c,
                       std::string& label) {
    if (line.empty() || line[0] != '#') return;
    size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    auto eq = line.find('=', i);
    if (eq == std::string::npos) return;
    std::string key   = line.substr(i, eq - i);
    std::string value = line.substr(eq + 1);
    trim(key);
    trim(value);
    if (key == "reference_temp_c") {
        try { reference_temp_c = std::stod(value); }
        catch (const std::exception&) {}
    } else if (key == "label") {
        label = value;
    }
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) cols.push_back(col);
    return cols;
}

} // namespace

ThermalCompensation::ThermalCompensation() = default;

ThermalCompensation::~ThermalCompensation() {
    stopPolling();
}

bool ThermalCompensation::loadFactorTable(const std::string& path) {
    table_loaded_ = false;
    temps_.clear();
    factors_.clear();
    label_.clear();
    source_path_ = path;
    reference_temp_c_ = 0.0;

    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_ERROR("ThermalCompensation", "Cannot open factor table: " << path);
        return false;
    }

    std::vector<std::pair<double, double>> rows;  // (temp_c, factor)
    std::string line;
    bool header_seen = false;
    while (std::getline(f, line)) {
        std::string trimmed = line;
        trim(trimmed);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') {
            parseMetadataLine(trimmed, reference_temp_c_, label_);
            continue;
        }
        // Skip the column header row, exactly once.
        if (!header_seen && trimmed.rfind("backlight_temp_c", 0) == 0) {
            header_seen = true;
            continue;
        }

        auto cols = splitCsv(trimmed);
        if (cols.size() < 2) continue;
        try {
            double t = std::stod(cols[0]);
            double fac = std::stod(cols[1]);
            if (fac <= 0.0) continue;     // bad row - skip
            rows.emplace_back(t, fac);
        } catch (const std::exception&) {
            continue;
        }
    }

    if (rows.empty()) {
        LOG_ERROR("ThermalCompensation", "No usable rows in " << path);
        return false;
    }

    // Sort ascending by temp; collapse exact duplicates by averaging factor.
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                  return a.first < b.first;
              });
    for (size_t i = 0; i < rows.size();) {
        size_t j = i;
        double sum = 0.0;
        while (j < rows.size() && rows[j].first == rows[i].first) {
            sum += rows[j].second;
            ++j;
        }
        temps_.push_back(rows[i].first);
        factors_.push_back(sum / static_cast<double>(j - i));
        i = j;
    }

    min_factor_ = *std::min_element(factors_.begin(), factors_.end());
    max_factor_ = *std::max_element(factors_.begin(), factors_.end());

    table_loaded_ = true;
    LOG_INFO("ThermalCompensation", "Loaded " << temps_.size() << " factor rows from "
             << path
             << " [temp " << temps_.front() << ".." << temps_.back() << " C"
             << " | factor " << min_factor_ << ".." << max_factor_ << "]"
             << " ref_temp_c=" << reference_temp_c_
             << (label_.empty() ? "" : " label=" + label_));
    return true;
}

void ThermalCompensation::configureI2cSource(const std::string& device,
                                             uint8_t i2c_address,
                                             uint16_t register_addr,
                                             double scale) {
    if (polling_started_) {
        LOG_WARN("ThermalCompensation", "configureI2cSource() called after polling already "
                 "started; ignoring (call before startPolling)");
        return;
    }
    i2c_device_ = device;
    i2c_address_ = i2c_address;
    i2c_register_ = register_addr;
    i2c_scale_ = scale;
}

void ThermalCompensation::startPolling(const std::string& temp_command,
                                       int poll_interval_sec) {
    if (polling_started_) {
        LOG_WARN("ThermalCompensation", "startPolling() called twice; ignoring");
        return;
    }
    if (temp_command.empty() && i2c_device_.empty()) {
        LOG_WARN("ThermalCompensation", "startPolling() called with neither temp_command "
                 "nor i2c_temp_source configured; thermal compensation will not be active");
        return;
    }
    if (!table_loaded_) {
        LOG_WARN("ThermalCompensation", "startPolling() called but no factor table is "
                 "loaded; thermal compensation will not be active");
        return;
    }
    temp_command_ = temp_command;
    poll_interval_sec_ = std::max(1, poll_interval_sec);
    stop_requested_.store(false);
    polling_started_ = true;
    poll_thread_ = std::thread(&ThermalCompensation::pollLoop, this);
    if (!i2c_device_.empty() && !temp_command_.empty()) {
        LOG_INFO("ThermalCompensation", "Polling started (interval " << poll_interval_sec_
                 << "s), I2C primary + command fallback");
    } else if (!i2c_device_.empty()) {
        LOG_INFO("ThermalCompensation", "Polling started (interval " << poll_interval_sec_
                 << "s), I2C source only");
    } else {
        LOG_INFO("ThermalCompensation", "Polling started (interval " << poll_interval_sec_
                 << "s), command source only");
    }
}

void ThermalCompensation::stopPolling() {
    if (!polling_started_) return;
    stop_requested_.store(true);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    polling_started_ = false;
    LOG_INFO("ThermalCompensation", "Polling stopped");
}

void ThermalCompensation::pollLoop() {
    // Run one check immediately on startup so the first factor() call after
    // a brief initialization window already has data instead of returning 1.0
    // for the entire first poll interval.
    runOneTempCheck();

    while (!stop_requested_.load()) {
        // Sleep in 1-second chunks so a daemon shutdown signal is acted on
        // within ~1 second instead of waiting for the next poll.
        for (int i = 0; i < poll_interval_sec_; ++i) {
            if (stop_requested_.load()) return;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stop_requested_.load()) return;
        runOneTempCheck();
    }
}

bool ThermalCompensation::readTempViaI2c(double* out) {
    if (i2c_device_.empty()) return false;

    int fd = open(i2c_device_.c_str(), O_RDWR);
    if (fd < 0) return false;

    // Bind the slave address. Reusing this fd for multiple ioctl calls is
    // fine - the I2C_SLAVE binding sticks until we close the fd.
    if (ioctl(fd, I2C_SLAVE, i2c_address_) < 0) {
        close(fd);
        return false;
    }

    // Atomic write-subaddress + read-2-bytes via I2C_RDWR. Both messages
    // run under the kernel's bus_lock as one transaction so other
    // userspace clients reading the same slave can't interleave between
    // the address-pointer write and the data read.
    uint8_t reg_buf[2] = {
        static_cast<uint8_t>((i2c_register_ >> 8) & 0xff),
        static_cast<uint8_t>(i2c_register_ & 0xff)
    };
    uint8_t data[2] = {0, 0};
    struct i2c_msg msgs[2];
    msgs[0].addr  = i2c_address_;
    msgs[0].flags = 0;          // write
    msgs[0].len   = 2;
    msgs[0].buf   = reg_buf;
    msgs[1].addr  = i2c_address_;
    msgs[1].flags = I2C_M_RD;   // read
    msgs[1].len   = 2;
    msgs[1].buf   = data;
    struct i2c_rdwr_ioctl_data xact;
    xact.msgs  = msgs;
    xact.nmsgs = 2;

    int rc = ioctl(fd, I2C_RDWR, &xact);
    close(fd);
    if (rc < 0) return false;

    // F1KM format: signed int16, big-endian on the wire, value * scale = degC.
    int16_t raw = static_cast<int16_t>((data[0] << 8) | data[1]);
    *out = static_cast<double>(raw) * i2c_scale_;
    return true;
}

bool ThermalCompensation::readTempViaCommand(double* out) {
    if (temp_command_.empty()) return false;

    // popen() runs through /bin/sh -c so shell pipelines work as written.
    // The command is responsible for its own stderr handling - we only
    // capture stdout. Same parsing convention as als-dimmer-sweep.py.
    FILE* p = popen(temp_command_.c_str(), "r");
    if (!p) return false;

    std::string buf;
    char chunk[256];
    while (fgets(chunk, sizeof(chunk), p)) buf.append(chunk);
    int rc = pclose(p);
    if (rc != 0) return false;

    double temp = parseTempFromOutput(buf);
    if (std::isnan(temp)) return false;
    *out = temp;
    return true;
}

void ThermalCompensation::runOneTempCheck() {
    // Try i2c first when configured. On per-cycle failure, fall through to
    // the temp_command path (when configured) so transient i2c hiccups don't
    // cause the daemon to lose temperature visibility.
    double temp = 0.0;
    bool got_via_i2c = false;
    bool i2c_configured = !i2c_device_.empty();
    bool cmd_configured = !temp_command_.empty();

    if (i2c_configured) {
        if (readTempViaI2c(&temp)) {
            got_via_i2c = true;
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            if (!warned_about_i2c_failure_) {
                warned_about_i2c_failure_ = true;
                LOG_WARN("ThermalCompensation", "I2C temp read failed at "
                         << i2c_device_ << " 0x"
                         << std::hex << static_cast<int>(i2c_address_) << std::dec
                         << "; will retry next cycle"
                         << (cmd_configured ? " (falling back to temp_command this cycle)"
                                            : ""));
            }
        }
    }

    bool got_via_command = false;
    if (!got_via_i2c && cmd_configured) {
        if (readTempViaCommand(&temp)) {
            got_via_command = true;
            // Note when the command path is being used as a fallback (i.e.
            // i2c was configured but failed). One-shot log so operators can
            // see the situation without flooding.
            if (i2c_configured) {
                std::lock_guard<std::mutex> lk(mu_);
                if (!logged_fallback_to_command_) {
                    logged_fallback_to_command_ = true;
                    LOG_WARN("ThermalCompensation", "Using temp_command fallback because "
                             "i2c_temp_source isn't returning data");
                }
            }
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            ++consecutive_failures_;
            if (consecutive_failures_ == 1) {
                LOG_WARN("ThermalCompensation", "temp_command read failed");
            }
        }
    }

    if (!got_via_i2c && !got_via_command) {
        std::lock_guard<std::mutex> lk(mu_);
        ++consecutive_failures_;
        return;
    }

    // Success path - cache the reading
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_temp_c_ = temp;
        last_read_time_ = std::chrono::steady_clock::now();
        bool first_time = !has_reading_;
        has_reading_ = true;
        consecutive_failures_ = 0;
        warned_about_sustained_failure_ = false;

        if (first_time) {
            LOG_INFO("ThermalCompensation", "First temp read OK: " << temp << " C "
                     << "(via " << (got_via_i2c ? "I2C" : "temp_command") << ")");
        }
        // If i2c just recovered from a previous failure, log that visibly
        if (got_via_i2c && warned_about_i2c_failure_) {
            warned_about_i2c_failure_ = false;
            logged_first_i2c_success_ = true;
            LOG_INFO("ThermalCompensation", "I2C temp source recovered: "
                     << temp << " C");
        }
    }
}

double ThermalCompensation::parseTempFromOutput(const std::string& output) {
    // Find the first signed decimal-or-integer token and parse it. Matches
    // the same approach als-dimmer-sweep.py uses for its --temp-cmd parsing
    // so users can reuse identical commands across the two tools.
    static const std::regex re(R"(-?\d+(\.\d+)?)");
    std::smatch m;
    if (std::regex_search(output, m, re)) {
        try {
            return std::stod(m[0].str());
        } catch (const std::exception&) {
            return std::nan("");
        }
    }
    return std::nan("");
}

double ThermalCompensation::interpolateFactorAt(double temp_c) const {
    if (temps_.empty()) return 1.0;
    if (temp_c <= temps_.front()) return factors_.front();
    if (temp_c >= temps_.back())  return factors_.back();
    auto it = std::lower_bound(temps_.begin(), temps_.end(), temp_c);
    size_t idx = static_cast<size_t>(it - temps_.begin());
    if (idx == 0) return factors_.front();
    double t0 = temps_[idx - 1], t1 = temps_[idx];
    double f0 = factors_[idx - 1], f1 = factors_[idx];
    if (t1 == t0) return (f0 + f1) / 2.0;
    return f0 + (temp_c - t0) / (t1 - t0) * (f1 - f0);
}

double ThermalCompensation::factor() const {
    if (!isEnabled()) return 1.0;

    std::lock_guard<std::mutex> lk(mu_);
    if (!has_reading_) return 1.0;

    // Staleness watchdog: if the last successful read is more than 5x the
    // poll interval old (capped at 5 minutes), distrust it and fall back
    // to no correction. This catches scenarios where the temp source
    // started failing AFTER having previously worked.
    auto stale_threshold_ms = std::min<int64_t>(
        static_cast<int64_t>(poll_interval_sec_) * 5 * 1000,
        5 * 60 * 1000);
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - last_read_time_).count();
    if (age > stale_threshold_ms) {
        if (!warned_about_sustained_failure_) {
            // Cast away const for the one-shot flag; mu_ is mutable so this
            // is safe. (The alternative is a separate atomic flag.)
            const_cast<ThermalCompensation*>(this)
                ->warned_about_sustained_failure_ = true;
            LOG_WARN("ThermalCompensation", "Last temp reading is " << age
                     << " ms old (>" << stale_threshold_ms
                     << " ms); falling back to no correction");
        }
        return 1.0;
    }

    return interpolateFactorAt(last_temp_c_);
}

bool ThermalCompensation::hasReading() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_reading_) return false;
    auto stale_threshold_ms = std::min<int64_t>(
        static_cast<int64_t>(poll_interval_sec_) * 5 * 1000,
        5 * 60 * 1000);
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - last_read_time_).count();
    return age <= stale_threshold_ms;
}

double ThermalCompensation::lastTempC() const {
    std::lock_guard<std::mutex> lk(mu_);
    return has_reading_ ? last_temp_c_ : std::nan("");
}

int64_t ThermalCompensation::lastReadAgeMs() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_reading_) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - last_read_time_).count();
}

} // namespace als_dimmer
