#include "als-dimmer/thermal_compensation.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

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

void ThermalCompensation::startPolling(const std::string& temp_command,
                                       int poll_interval_sec) {
    if (polling_started_) {
        LOG_WARN("ThermalCompensation", "startPolling() called twice; ignoring");
        return;
    }
    if (temp_command.empty()) {
        LOG_WARN("ThermalCompensation", "startPolling() called with empty temp_command; "
                 "thermal compensation will not be active");
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
    LOG_INFO("ThermalCompensation", "Polling started (interval " << poll_interval_sec_ << "s)");
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

void ThermalCompensation::runOneTempCheck() {
    // popen() runs through /bin/sh -c so shell pipelines (which is how most
    // temp sources are expressed) work as written. The command is responsible
    // for its own stderr handling - we only capture stdout.
    FILE* p = popen(temp_command_.c_str(), "r");
    if (!p) {
        std::lock_guard<std::mutex> lk(mu_);
        ++consecutive_failures_;
        if (consecutive_failures_ == 1) {
            LOG_WARN("ThermalCompensation", "popen() failed for temp_command");
        }
        return;
    }

    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) {
        out.append(buf);
    }
    int rc = pclose(p);
    if (rc != 0) {
        std::lock_guard<std::mutex> lk(mu_);
        ++consecutive_failures_;
        if (consecutive_failures_ == 1) {
            LOG_WARN("ThermalCompensation", "temp_command exited rc=" << rc);
        }
        return;
    }

    double temp = parseTempFromOutput(out);
    if (std::isnan(temp)) {
        std::lock_guard<std::mutex> lk(mu_);
        ++consecutive_failures_;
        if (consecutive_failures_ == 1) {
            LOG_WARN("ThermalCompensation", "could not parse temp from temp_command output");
        }
        return;
    }

    // Success path
    bool was_warned = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_temp_c_ = temp;
        last_read_time_ = std::chrono::steady_clock::now();
        bool first_time = !has_reading_;
        has_reading_ = true;
        was_warned = warned_about_sustained_failure_;
        consecutive_failures_ = 0;
        warned_about_sustained_failure_ = false;
        if (first_time) {
            LOG_INFO("ThermalCompensation", "First temp read OK: " << temp << " C");
        }
    }
    (void)was_warned;
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
