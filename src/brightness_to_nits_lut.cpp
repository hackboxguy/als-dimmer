#include "als-dimmer/brightness_to_nits_lut.hpp"
#include "als-dimmer/logger.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace als_dimmer {

namespace {

void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    if (start > 0) s.erase(0, start);
}

// Parse "# key=value" metadata lines. Returns true if line was a metadata
// comment (consumed); false otherwise.
bool parseMetadataLine(const std::string& line,
                       std::string& output_type_tag,
                       std::string& label) {
    if (line.empty() || line[0] != '#') return false;

    // Strip leading "# " or "#"
    size_t i = 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

    auto eq = line.find('=', i);
    if (eq == std::string::npos) return true;  // it's a comment, just no kv

    std::string key   = line.substr(i, eq - i);
    std::string value = line.substr(eq + 1);
    trim(key);
    trim(value);

    if (key == "output_type") output_type_tag = value;
    else if (key == "label")  label           = value;
    return true;
}

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, ',')) {
        cols.push_back(col);
    }
    return cols;
}

} // namespace

bool BrightnessToNitsLut::loadFromFile(const std::string& path) {
    loaded_ = false;
    pct_.clear();
    nits_.clear();
    nits_sorted_.clear();
    pct_for_nits_sorted_.clear();
    output_type_tag_.clear();
    label_.clear();
    source_path_ = path;

    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_ERROR("BrightnessToNitsLut", "Cannot open " << path);
        return false;
    }

    std::vector<std::pair<double, double>> rows;  // (pct, nits)
    std::string line;
    while (std::getline(f, line)) {
        std::string trimmed = line;
        trim(trimmed);
        if (trimmed.empty()) continue;

        if (trimmed[0] == '#') {
            parseMetadataLine(trimmed, output_type_tag_, label_);
            continue;
        }

        // Skip header row
        if (trimmed.rfind("brightness_pct", 0) == 0) continue;

        auto cols = splitCsv(trimmed);
        if (cols.size() < 2) continue;

        // Optional status column - if present, only OK rows count.
        if (cols.size() >= 3) {
            std::string status = cols[2];
            trim(status);
            if (!status.empty() && status != "OK") continue;
        }

        try {
            double pct  = std::stod(cols[0]);
            double nits = std::stod(cols[1]);
            if (pct < 0.0 || pct > 100.0) continue;  // out-of-range row
            if (nits < 0.0) continue;
            rows.emplace_back(pct, nits);
        } catch (const std::exception&) {
            continue;
        }
    }

    if (rows.empty()) {
        LOG_ERROR("BrightnessToNitsLut", "No usable rows in " << path);
        return false;
    }

    // Sort by pct ascending; collapse duplicate pct values by averaging nits.
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                  return a.first < b.first;
              });

    for (size_t i = 0; i < rows.size(); ) {
        size_t j = i;
        double sum = 0.0;
        while (j < rows.size() && rows[j].first == rows[i].first) {
            sum += rows[j].second;
            ++j;
        }
        pct_.push_back(rows[i].first);
        nits_.push_back(sum / static_cast<double>(j - i));
        i = j;
    }

    min_pct_ = pct_.front();
    max_pct_ = pct_.back();

    // Build the reverse-lookup arrays. Note: the curve is usually monotonic,
    // but if the panel/output has a flat region, sorting by nits + linear
    // interp gives a reasonable inverse. Duplicates collapsed similarly.
    std::vector<std::pair<double, double>> by_nits;  // (nits, pct)
    by_nits.reserve(pct_.size());
    for (size_t k = 0; k < pct_.size(); ++k) {
        by_nits.emplace_back(nits_[k], pct_[k]);
    }
    std::sort(by_nits.begin(), by_nits.end(),
              [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                  return a.first < b.first;
              });
    for (size_t i = 0; i < by_nits.size(); ) {
        size_t j = i;
        double sum_pct = 0.0;
        while (j < by_nits.size() && by_nits[j].first == by_nits[i].first) {
            sum_pct += by_nits[j].second;
            ++j;
        }
        nits_sorted_.push_back(by_nits[i].first);
        pct_for_nits_sorted_.push_back(sum_pct / static_cast<double>(j - i));
        i = j;
    }

    min_nits_ = nits_sorted_.front();
    max_nits_ = nits_sorted_.back();

    loaded_ = true;
    LOG_INFO("BrightnessToNitsLut", "Loaded " << pct_.size() << " rows from " << path
             << " [pct " << min_pct_ << ".." << max_pct_
             << " | nits " << min_nits_ << ".." << max_nits_ << "]"
             << (output_type_tag_.empty() ? "" : " output_type=" + output_type_tag_)
             << (label_.empty()           ? "" : " label="       + label_));
    return true;
}

double BrightnessToNitsLut::pctToNits(double brightness_pct, bool& clamped) const {
    clamped = false;
    if (brightness_pct <= min_pct_) {
        if (brightness_pct < min_pct_) clamped = true;
        return nits_.front();
    }
    if (brightness_pct >= max_pct_) {
        if (brightness_pct > max_pct_) clamped = true;
        return nits_.back();
    }
    auto it = std::lower_bound(pct_.begin(), pct_.end(), brightness_pct);
    size_t idx = static_cast<size_t>(it - pct_.begin());
    if (idx == 0) return nits_.front();
    double p0 = pct_[idx - 1], p1 = pct_[idx];
    double n0 = nits_[idx - 1], n1 = nits_[idx];
    if (p1 == p0) return (n0 + n1) / 2.0;
    return n0 + (brightness_pct - p0) / (p1 - p0) * (n1 - n0);
}

double BrightnessToNitsLut::nitsToPct(double target_nits, bool& clamped) const {
    clamped = false;
    if (target_nits <= min_nits_) {
        if (target_nits < min_nits_) clamped = true;
        return pct_for_nits_sorted_.front();
    }
    if (target_nits >= max_nits_) {
        if (target_nits > max_nits_) clamped = true;
        return pct_for_nits_sorted_.back();
    }
    auto it = std::lower_bound(nits_sorted_.begin(), nits_sorted_.end(), target_nits);
    size_t idx = static_cast<size_t>(it - nits_sorted_.begin());
    if (idx == 0) return pct_for_nits_sorted_.front();
    double n0 = nits_sorted_[idx - 1], n1 = nits_sorted_[idx];
    double p0 = pct_for_nits_sorted_[idx - 1], p1 = pct_for_nits_sorted_[idx];
    if (n1 == n0) return (p0 + p1) / 2.0;
    return p0 + (target_nits - n0) / (n1 - n0) * (p1 - p0);
}

} // namespace als_dimmer
