#include "als-dimmer/notifier.hpp"
#include "als-dimmer/logger.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cstdlib>

namespace als_dimmer {

Notifier::Notifier(const NotificationConfig& config)
    : config_(config)
    , last_brightness_emit_time_(std::chrono::steady_clock::time_point::min()) {
}

bool Notifier::isEnabled() const {
    return config_.enabled && !config_.on_change_script.empty();
}

void Notifier::emitModeChanged(const std::string& mode) {
    if (!isEnabled()) return;
    if (mode == last_mode_) return;

    last_mode_ = mode;
    invokeScript("mode_changed", mode);
}

void Notifier::emitBrightnessChanged(int brightness) {
    if (!isEnabled()) return;
    if (brightness == last_brightness_) return;

    // Rate limit: at most once per second
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_brightness_emit_time_).count();
    if (elapsed < 1000) return;

    last_brightness_ = brightness;
    last_brightness_emit_time_ = now;
    invokeScript("brightness_changed", std::to_string(brightness));
}

void Notifier::emitZoneChanged(const std::string& zone) {
    if (!isEnabled()) return;
    if (zone == last_zone_) return;

    last_zone_ = zone;
    invokeScript("zone_changed", zone);
}

void Notifier::invokeScript(const std::string& event_type, const std::string& value) {
    LOG_DEBUG("Notifier", "Emitting " << event_type << " = " << value);

    // Fire-and-forget via fork+exec.
    // Double-fork to avoid zombies: the intermediate child exits immediately,
    // and the grandchild is reparented to init/systemd.
    pid_t pid = fork();
    if (pid < 0) {
        LOG_WARN("Notifier", "fork() failed: " << strerror(errno));
        return;
    }

    if (pid == 0) {
        // First child — fork again and exit immediately
        pid_t pid2 = fork();
        if (pid2 < 0) {
            _exit(1);
        }
        if (pid2 > 0) {
            // Intermediate child exits, grandchild continues
            _exit(0);
        }

        // Grandchild — exec the callback script
        execl(config_.on_change_script.c_str(),
              config_.on_change_script.c_str(),
              event_type.c_str(),
              value.c_str(),
              nullptr);

        // exec failed — log to stderr and exit (can't use LOG in forked child safely)
        _exit(1);
    }

    // Parent — reap the intermediate child (exits immediately, so non-blocking in practice)
    int status;
    waitpid(pid, &status, 0);
}

} // namespace als_dimmer
