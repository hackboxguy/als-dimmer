#include "als-dimmer/outputs/boe_pwm_output.hpp"
#include "als-dimmer/logger.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace als_dimmer {

BoePwmOutput::BoePwmOutput(const std::string& i2c_device,
                           uint8_t i2c_address,
                           const std::string& pwm_chip,
                           int pwm_channel,
                           int pwm_gpio,
                           const std::string& pwm_alt_func,
                           int period_ns,
                           const std::string& response_curve_path,
                           bool skip_chip_config)
    : i2c_device_(i2c_device)
    , i2c_address_(i2c_address)
    , pwm_chip_(pwm_chip)
    , pwm_channel_(pwm_channel)
    , pwm_gpio_(pwm_gpio)
    , pwm_alt_func_(pwm_alt_func)
    , period_ns_(period_ns)
    , response_curve_path_(response_curve_path)
    , skip_chip_config_(skip_chip_config)
    , i2c_fd_(-1)
    , current_brightness_(-1)
    , last_duty_ns_(-1)
    , curve_max_nits_(0.0)
    , curve_loaded_(false) {

    pwm_chip_dir_    = "/sys/class/pwm/" + pwm_chip_;
    pwm_channel_dir_ = pwm_chip_dir_ + "/pwm" + std::to_string(pwm_channel_);
    duty_cycle_path_ = pwm_channel_dir_ + "/duty_cycle";
}

BoePwmOutput::~BoePwmOutput() {
    if (i2c_fd_ >= 0) {
        close(i2c_fd_);
        i2c_fd_ = -1;
    }
}

std::string BoePwmOutput::getType() const {
    return "boe_pwm";
}

int BoePwmOutput::getCurrentBrightness() {
    // PWM/MPQ3367 setup doesn't give a meaningful readback; cache instead.
    return current_brightness_ < 0 ? 0 : current_brightness_;
}

bool BoePwmOutput::init() {
    if (!skip_chip_config_) {
        if (!configureMpq3367()) {
            return false;
        }
    } else {
        LOG_INFO("BoePwmOutput", "skip_chip_config=true, not touching MPQ3367 I2C config");
    }

    assertPinMux();

    if (!setupPwm()) {
        return false;
    }

    if (!response_curve_path_.empty()) {
        if (!loadResponseCurve()) {
            LOG_WARN("BoePwmOutput", "Failed to load response curve; falling back to direct duty mapping");
        }
    }

    LOG_INFO("BoePwmOutput", "Initialized: i2c=" << i2c_device_
             << " addr=0x" << std::hex << static_cast<int>(i2c_address_) << std::dec
             << " pwm=" << pwm_channel_dir_
             << " period=" << period_ns_ << "ns"
             << " gpio=" << pwm_gpio_ << " (" << pwm_alt_func_ << ")"
             << " curve=" << (curve_loaded_ ? response_curve_path_ : std::string("(none)")));
    return true;
}

bool BoePwmOutput::configureMpq3367() {
    i2c_fd_ = open(i2c_device_.c_str(), O_RDWR);
    if (i2c_fd_ < 0) {
        LOG_ERROR("BoePwmOutput", "Failed to open " << i2c_device_ << ": " << std::strerror(errno));
        return false;
    }
    if (ioctl(i2c_fd_, I2C_SLAVE, i2c_address_) < 0) {
        LOG_ERROR("BoePwmOutput", "Failed to set I2C slave 0x"
                  << std::hex << static_cast<int>(i2c_address_) << std::dec
                  << ": " << std::strerror(errno));
        close(i2c_fd_);
        i2c_fd_ = -1;
        return false;
    }

    // Reg 0x00 = 0xA6 : OTID=1, MODE=01 (pure PWM), FSPMF=11
    // Reg 0x01 = 0xAA : PSE=1, TH_S=01 (5V), FSW=01 (400kHz), CH=010 (4 strings)
    auto write_reg = [&](uint8_t reg, uint8_t val) -> bool {
        uint8_t buf[2] = {reg, val};
        if (write(i2c_fd_, buf, 2) != 2) {
            LOG_ERROR("BoePwmOutput", "I2C write reg 0x"
                      << std::hex << static_cast<int>(reg) << " failed: "
                      << std::dec << std::strerror(errno));
            return false;
        }
        return true;
    };

    if (!write_reg(0x00, 0xA6)) return false;
    if (!write_reg(0x01, 0xAA)) return false;

    // Clear FT_LEDO power-on latch by reading 0x02 twice.
    auto read_reg = [&](uint8_t reg, uint8_t& out) -> bool {
        if (write(i2c_fd_, &reg, 1) != 1) return false;
        if (read(i2c_fd_, &out, 1) != 1) return false;
        return true;
    };
    uint8_t v;
    (void)read_reg(0x02, v);
    (void)read_reg(0x02, v);

    LOG_DEBUG("BoePwmOutput", "MPQ3367 configured: 0x00<-0xA6, 0x01<-0xAA, FT_LEDO latch cleared");
    return true;
}

void BoePwmOutput::assertPinMux() {
    // Shell out to pinctrl. Failure here is non-fatal: dtoverlay=pwm in
    // /boot/firmware/config.txt is the authoritative source. We log and move on.
    std::ostringstream cmd;
    cmd << "pinctrl set " << pwm_gpio_ << " " << pwm_alt_func_ << " 2>&1";
    FILE* p = popen(cmd.str().c_str(), "r");
    if (!p) {
        LOG_WARN("BoePwmOutput", "popen pinctrl failed: " << std::strerror(errno)
                 << " (relying on dtoverlay)");
        return;
    }
    std::string out;
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) {
        out.append(buf);
    }
    int rc = pclose(p);
    if (rc != 0) {
        LOG_WARN("BoePwmOutput", "pinctrl set " << pwm_gpio_ << " " << pwm_alt_func_
                 << " exited rc=" << rc
                 << (out.empty() ? "" : ": " + out)
                 << " (relying on dtoverlay)");
    } else {
        LOG_DEBUG("BoePwmOutput", "pinctrl set " << pwm_gpio_ << " " << pwm_alt_func_ << " OK");
    }
}

bool BoePwmOutput::writeSysfs(const std::string& path, const std::string& value) {
    std::ofstream f(path);
    if (!f.is_open()) {
        LOG_ERROR("BoePwmOutput", "open " << path << " for write failed: " << std::strerror(errno));
        return false;
    }
    f << value;
    f.flush();
    if (!f.good()) {
        LOG_ERROR("BoePwmOutput", "write '" << value << "' to " << path << " failed");
        return false;
    }
    return true;
}

static bool dirExists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool BoePwmOutput::setupPwm() {
    if (!dirExists(pwm_chip_dir_)) {
        LOG_ERROR("BoePwmOutput", pwm_chip_dir_ << " not found - is dtoverlay=pwm loaded in /boot/firmware/config.txt?");
        return false;
    }

    if (!dirExists(pwm_channel_dir_)) {
        if (!writeSysfs(pwm_chip_dir_ + "/export", std::to_string(pwm_channel_))) {
            return false;
        }
        // Wait up to ~2s for the channel directory to appear (udev may chgrp it).
        for (int i = 0; i < 40; ++i) {
            if (dirExists(pwm_channel_dir_)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!dirExists(pwm_channel_dir_)) {
            LOG_ERROR("BoePwmOutput", pwm_channel_dir_ << " did not appear after export");
            return false;
        }
    }

    // Zero duty before setting period - kernel rejects period < current duty.
    if (!writeSysfs(duty_cycle_path_, "0")) return false;
    if (!writeSysfs(pwm_channel_dir_ + "/period", std::to_string(period_ns_))) return false;
    if (!writeSysfs(pwm_channel_dir_ + "/enable", "1")) return false;

    last_duty_ns_ = 0;
    return true;
}

bool BoePwmOutput::loadResponseCurve() {
    std::ifstream f(response_curve_path_);
    if (!f.is_open()) {
        LOG_ERROR("BoePwmOutput", "Cannot open response curve: " << response_curve_path_);
        return false;
    }

    std::vector<std::pair<double, double>> points;  // (nits, duty_pct)
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("duty_pct", 0) == 0) continue;  // header

        // Expected: duty_pct,duty_ns,Y_nits,retries,status
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string col;
        while (std::getline(ss, col, ',')) {
            cols.push_back(col);
        }
        if (cols.size() < 5) continue;

        // Trim status field
        std::string status = cols[4];
        while (!status.empty() && (status.back() == '\r' || status.back() == ' ' || status.back() == '\n')) {
            status.pop_back();
        }
        if (status != "OK") continue;

        try {
            double duty_pct = std::stod(cols[0]);
            double y_nits   = std::stod(cols[2]);
            points.emplace_back(y_nits, duty_pct);
        } catch (const std::exception&) {
            continue;
        }
    }

    if (points.empty()) {
        LOG_ERROR("BoePwmOutput", "No OK rows in response curve: " << response_curve_path_);
        return false;
    }

    std::sort(points.begin(), points.end(),
              [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                  return a.first < b.first;
              });

    curve_nits_.clear();
    curve_duty_pct_.clear();
    curve_nits_.reserve(points.size());
    curve_duty_pct_.reserve(points.size());
    for (const auto& p : points) {
        curve_nits_.push_back(p.first);
        curve_duty_pct_.push_back(p.second);
    }
    curve_max_nits_ = curve_nits_.back();
    curve_loaded_ = true;

    LOG_INFO("BoePwmOutput", "Loaded response curve: " << points.size() << " points, "
             << curve_nits_.front() << ".." << curve_max_nits_ << " nits");
    return true;
}

double BoePwmOutput::brightnessToDutyPct(int brightness) const {
    brightness = std::max(0, std::min(100, brightness));

    if (!curve_loaded_) {
        return static_cast<double>(brightness);
    }

    // Linear-in-nits: target_nits = brightness/100 * max_nits, then inverse interp.
    double target_nits = (static_cast<double>(brightness) / 100.0) * curve_max_nits_;
    auto it = std::lower_bound(curve_nits_.begin(), curve_nits_.end(), target_nits);
    if (it == curve_nits_.begin()) {
        return curve_duty_pct_.front();
    }
    if (it == curve_nits_.end()) {
        return curve_duty_pct_.back();
    }
    size_t idx = static_cast<size_t>(it - curve_nits_.begin());
    double n0 = curve_nits_[idx - 1];
    double n1 = curve_nits_[idx];
    double d0 = curve_duty_pct_[idx - 1];
    double d1 = curve_duty_pct_[idx];
    if (n1 == n0) return (d0 + d1) / 2.0;
    return d0 + (target_nits - n0) / (n1 - n0) * (d1 - d0);
}

int BoePwmOutput::dutyPctToNs(double duty_pct) const {
    if (duty_pct >= 100.0) return period_ns_ - 1;  // BCM2711 period-1 quirk at 100%
    if (duty_pct <= 0.0)   return 0;
    long ns = static_cast<long>((static_cast<double>(period_ns_) * duty_pct / 100.0) + 0.5);
    if (ns < 0) ns = 0;
    if (ns >= period_ns_) ns = period_ns_ - 1;
    return static_cast<int>(ns);
}

bool BoePwmOutput::setBrightness(int brightness) {
    brightness = std::max(0, std::min(100, brightness));

    int duty_ns;
    if (brightness == 0) {
        duty_ns = 0;
    } else if (brightness == 100) {
        duty_ns = period_ns_ - 1;
    } else {
        double duty_pct = brightnessToDutyPct(brightness);
        duty_ns = dutyPctToNs(duty_pct);
    }

    if (duty_ns == last_duty_ns_) {
        current_brightness_ = brightness;
        return true;
    }

    if (!writeSysfs(duty_cycle_path_, std::to_string(duty_ns))) {
        return false;
    }

    last_duty_ns_ = duty_ns;
    current_brightness_ = brightness;
    LOG_TRACE("BoePwmOutput", "brightness=" << brightness << "% -> duty=" << duty_ns << "ns");
    return true;
}

std::unique_ptr<OutputInterface> createBoePwmOutput(const std::string& i2c_device,
                                                    uint8_t i2c_address,
                                                    const std::string& pwm_chip,
                                                    int pwm_channel,
                                                    int pwm_gpio,
                                                    const std::string& pwm_alt_func,
                                                    int period_ns,
                                                    const std::string& response_curve_path,
                                                    bool skip_chip_config) {
    return std::unique_ptr<OutputInterface>(new BoePwmOutput(
        i2c_device, i2c_address, pwm_chip, pwm_channel, pwm_gpio,
        pwm_alt_func, period_ns, response_curve_path, skip_chip_config));
}

} // namespace als_dimmer
