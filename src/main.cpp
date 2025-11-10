#include "als-dimmer/config.hpp"
#include "als-dimmer/interfaces.hpp"
#include "als-dimmer/state_manager.hpp"
#include "als-dimmer/control_interface.hpp"
#include "als-dimmer/zone_mapper.hpp"
#include "als-dimmer/brightness_controller.hpp"
#include "als-dimmer/csv_logger.hpp"
#include "als-dimmer/logger.hpp"
#include "als-dimmer/json_protocol.hpp"
#include "als-dimmer/sensors/can_als_sensor.hpp"
#include "json.hpp"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <iomanip>

using json = nlohmann::json;

namespace als_dimmer {

// Forward declarations
std::unique_ptr<SensorInterface> createFileSensor(const std::string& file_path);
std::unique_ptr<SensorInterface> createOPTI4001Sensor(const std::string& device, const std::string& address);
std::unique_ptr<SensorInterface> createFPGAOpti4001Sensor(const std::string& device, const std::string& address);

std::unique_ptr<OutputInterface> createFileOutput(const std::string& file_path);
#ifdef HAVE_DDCUTIL
std::unique_ptr<OutputInterface> createDDCUtilOutput(int display_number);
#endif
std::unique_ptr<OutputInterface> createI2CDimmerOutput(const std::string& device,
                                                        uint8_t address,
                                                        const std::string& type);

} // namespace als_dimmer

// Factory functions
std::unique_ptr<als_dimmer::SensorInterface> createSensor(const als_dimmer::Config& config) {
    if (config.sensor.type == "file") {
        return als_dimmer::createFileSensor(config.sensor.file_path);
    } else if (config.sensor.type == "opti4001") {
        return als_dimmer::createOPTI4001Sensor(config.sensor.device, config.sensor.address);
    } else if (config.sensor.type == "fpga_opti4001") {
        return als_dimmer::createFPGAOpti4001Sensor(config.sensor.device, config.sensor.address);
    } else if (config.sensor.type == "can_als") {
        // Parse CAN ID from hex string (e.g., "0x0A2" -> 0x0A2)
        uint32_t can_id = static_cast<uint32_t>(std::stoul(config.sensor.can_id, nullptr, 16));
        return std::make_unique<als_dimmer::CANALSSensor>(
            config.sensor.can_interface,
            can_id,
            config.sensor.timeout_ms
        );
    }
    LOG_ERROR("factory", "Unsupported sensor type: " << config.sensor.type);
    return nullptr;
}

std::unique_ptr<als_dimmer::OutputInterface> createOutput(const als_dimmer::Config& config) {
    if (config.output.type == "file") {
        return als_dimmer::createFileOutput(config.output.file_path);
    }
#ifdef HAVE_DDCUTIL
    else if (config.output.type == "ddcutil") {
        return als_dimmer::createDDCUtilOutput(config.output.display_number);
    }
#endif
    else if (config.output.type == "dimmer200" || config.output.type == "dimmer800") {
        // Parse I2C address from hex string (e.g., "0x1D" -> 0x1D)
        uint8_t address = static_cast<uint8_t>(std::stoul(config.output.address, nullptr, 16));
        return als_dimmer::createI2CDimmerOutput(config.output.device, address, config.output.type);
    }
    LOG_ERROR("factory", "Unsupported output type: " << config.output.type);
    return nullptr;
}

// Legacy simple mapping (kept for backward compatibility with configs without zones)
int mapLuxToBrightnessSimple(float lux) {
    if (lux < 0) return 5;
    if (lux >= 1000) return 100;
    return 5 + static_cast<int>((lux / 1000.0f) * 95.0f);
}

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n";
    std::cout << "\nOPTIONS:\n";
    std::cout << "  --config <path>      Path to JSON config file (required)\n";
    std::cout << "  --log-level <level>  Set log level: trace, debug, info, warn, error (default: info)\n";
    std::cout << "  --foreground         Don't daemonize, log to console\n";
    std::cout << "  --csvlog <path>      Enable CSV logging to specified file\n";
    std::cout << "  --help               Show this help message\n";
    std::cout << "\nEXAMPLE:\n";
    std::cout << "  " << program_name << " --config configs/config_simulation.json --foreground\n";
    std::cout << "  " << program_name << " --config configs/config_simulation.json --log-level debug\n";
    std::cout << "  " << program_name << " --config configs/config.json --csvlog /tmp/data.csv --foreground\n";
}

// Process TCP commands
std::string processCommand(const std::string& command,
                          als_dimmer::StateManager& state_mgr,
                          als_dimmer::ControlInterface& control,
                          float current_lux,
                          int current_brightness,
                          std::chrono::steady_clock::time_point& manual_temp_start,
                          als_dimmer::ZoneMapper* zone_mapper) {
    (void)control;  // Reserved for future use (broadcasting status updates)

    using namespace als_dimmer::protocol;

    // Check if this is a JSON command (starts with '{')
    if (!command.empty() && command[0] == '{') {
        try {
            // Parse JSON command
            ParsedCommand parsed_cmd = parseCommand(command);

            // Process JSON command based on type
            switch (parsed_cmd.type) {
                case CommandType::GET_STATUS: {
                    std::string zone_name;
                    if (zone_mapper) {
                        zone_name = zone_mapper->getCurrentZoneName(current_lux);
                    } else {
                        zone_name = "simple";
                    }

                    // Convert internal OperatingMode to string for status response
                    std::string mode_str;
                    als_dimmer::OperatingMode current_mode = state_mgr.getMode();
                    switch (current_mode) {
                        case als_dimmer::OperatingMode::AUTO:
                            mode_str = "auto";
                            break;
                        case als_dimmer::OperatingMode::MANUAL:
                            mode_str = "manual";
                            break;
                        case als_dimmer::OperatingMode::MANUAL_TEMPORARY:
                            mode_str = "manual_temporary";
                            break;
                    }

                    return generateStatusResponse(
                        mode_str,
                        current_brightness,
                        current_lux,
                        zone_name
                    );
                }

                case CommandType::SET_MODE: {
                    if (!parsed_cmd.params.contains("mode")) {
                        return generateErrorResponse("Missing 'mode' parameter", "INVALID_PARAMS");
                    }

                    std::string mode_str = parsed_cmd.params["mode"].get<std::string>();
                    if (mode_str != "auto" && mode_str != "manual") {
                        return generateErrorResponse("Mode must be 'auto' or 'manual'", "INVALID_PARAMS");
                    }

                    auto new_mode = als_dimmer::StateManager::stringToMode(mode_str);
                    state_mgr.setMode(new_mode);
                    LOG_INFO("main", "Mode set to: " << mode_str << " (JSON)");

                    json data;
                    data["mode"] = mode_str;
                    return generateResponse(ResponseStatus::SUCCESS,
                                           "Mode set successfully", data);
                }

                case CommandType::SET_BRIGHTNESS: {
                    if (!parsed_cmd.params.contains("brightness")) {
                        return generateErrorResponse("Missing 'brightness' parameter", "INVALID_PARAMS");
                    }

                    int brightness = parsed_cmd.params["brightness"].get<int>();
                    if (brightness < 0 || brightness > 100) {
                        return generateErrorResponse("Brightness must be 0-100", "INVALID_PARAMS");
                    }

                    state_mgr.setManualBrightness(brightness);

                    // If in AUTO mode, switch to MANUAL_TEMPORARY
                    if (state_mgr.getMode() == als_dimmer::OperatingMode::AUTO) {
                        state_mgr.setMode(als_dimmer::OperatingMode::MANUAL_TEMPORARY);
                        manual_temp_start = std::chrono::steady_clock::now();
                        LOG_INFO("main", "Switched to MANUAL_TEMPORARY mode (JSON)");
                    } else if (state_mgr.getMode() == als_dimmer::OperatingMode::MANUAL_TEMPORARY) {
                        manual_temp_start = std::chrono::steady_clock::now();
                    }

                    json data;
                    data["brightness"] = brightness;
                    return generateResponse(ResponseStatus::SUCCESS,
                                           "Brightness set successfully", data);
                }

                case CommandType::ADJUST_BRIGHTNESS: {
                    if (!parsed_cmd.params.contains("delta")) {
                        return generateErrorResponse("Missing 'delta' parameter", "INVALID_PARAMS");
                    }

                    int delta = parsed_cmd.params["delta"].get<int>();
                    int current = state_mgr.getManualBrightness();
                    int new_brightness = std::max(0, std::min(100, current + delta));

                    state_mgr.setManualBrightness(new_brightness);

                    if (state_mgr.getMode() == als_dimmer::OperatingMode::AUTO) {
                        state_mgr.setMode(als_dimmer::OperatingMode::MANUAL_TEMPORARY);
                        manual_temp_start = std::chrono::steady_clock::now();
                    } else if (state_mgr.getMode() == als_dimmer::OperatingMode::MANUAL_TEMPORARY) {
                        manual_temp_start = std::chrono::steady_clock::now();
                    }

                    json data;
                    data["brightness"] = new_brightness;
                    data["delta"] = delta;
                    return generateResponse(ResponseStatus::SUCCESS,
                                           "Brightness adjusted successfully", data);
                }

                case CommandType::GET_CONFIG: {
                    // Return current configuration
                    json data;
                    data["mode"] = als_dimmer::StateManager::modeToString(state_mgr.getMode());
                    data["manual_brightness"] = state_mgr.getManualBrightness();
                    data["last_auto_brightness"] = state_mgr.getLastAutoBrightness();
                    return generateConfigResponse(data);
                }

                case CommandType::UNKNOWN:
                default:
                    return generateErrorResponse("Unknown command type", "UNKNOWN_COMMAND");
            }

        } catch (const json::parse_error& e) {
            return generateErrorResponse(
                std::string("JSON parse error: ") + e.what(), "PARSE_ERROR");
        } catch (const std::exception& e) {
            return generateErrorResponse(
                std::string("Internal error: ") + e.what(), "INTERNAL_ERROR");
        }
    }

    // Non-JSON command - return error
    return generateErrorResponse(
        "Invalid command format. Only JSON protocol is supported. "
        "Please send commands in JSON format starting with '{'",
        "INVALID_FORMAT");
}

int main(int argc, char* argv[]) {
    std::string config_file;
    std::string log_level_override;
    std::string csv_file;
    bool foreground = false;
    (void)foreground;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--log-level" && i + 1 < argc) {
            log_level_override = argv[++i];
        } else if (arg == "--csvlog" && i + 1 < argc) {
            csv_file = argv[++i];
        } else if (arg == "--foreground") {
            foreground = true;
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (config_file.empty()) {
        std::cerr << "Error: --config is required\n\n";
        printUsage(argv[0]);
        return 1;
    }

    // Load configuration
    als_dimmer::Config config;
    try {
        config = als_dimmer::Config::loadFromFile(config_file);
    } catch (const als_dimmer::ConfigError& e) {
        std::cerr << "Configuration error: " << e.what() << "\n";
        return 1;
    }

    // Initialize logger (command-line overrides config file)
    std::string log_level_str = log_level_override.empty() ? config.control.log_level : log_level_override;
    als_dimmer::Logger::getInstance().setLevel(als_dimmer::Logger::stringToLevel(log_level_str));

    LOG_INFO("main", "ALS-Dimmer starting (log level: " << log_level_str << ")");
    LOG_INFO("main", "Configuration loaded from " << config_file);

    // Initialize zone mapper (if zones are configured)
    std::unique_ptr<als_dimmer::ZoneMapper> zone_mapper;
    if (!config.zones.empty()) {
        try {
            zone_mapper = std::make_unique<als_dimmer::ZoneMapper>(
                config.zones, config.control.hysteresis_percent);
            LOG_INFO("main", "Zone mapper initialized with " << config.zones.size() << " zones"
                     << " (hysteresis: " << config.control.hysteresis_percent << "%)");
        } catch (const std::exception& e) {
            LOG_ERROR("main", "Failed to initialize zone mapper: " << e.what());
            return 1;
        }
    } else {
        LOG_INFO("main", "No zones configured, using simple linear mapping");
    }

    // Initialize brightness controller for smooth transitions
    als_dimmer::BrightnessController brightness_ctrl;
    LOG_DEBUG("main", "Brightness controller initialized (smooth transitions enabled)");

    // Initialize state manager
    als_dimmer::StateManager state_mgr(config.control.state_file);
    state_mgr.load();

    // Create sensor
    auto sensor = createSensor(config);
    if (!sensor || !sensor->init()) {
        LOG_ERROR("main", "Failed to initialize sensor");
        return 1;
    }
    LOG_INFO("main", "Sensor initialized: " << sensor->getType());

    // Create output
    auto output = createOutput(config);
    if (!output || !output->init()) {
        LOG_ERROR("main", "Failed to initialize output");
        return 1;
    }
    LOG_INFO("main", "Output initialized: " << output->getType());

    // Initialize control interface (TCP and/or Unix sockets)
    als_dimmer::ControlInterface control(config.control);
    if (!control.start()) {
        LOG_ERROR("main", "Failed to start control interface");
        return 1;
    }

    // Initialize CSV logger if requested
    std::unique_ptr<als_dimmer::CSVLogger> csv_logger;
    if (!csv_file.empty()) {
        csv_logger = std::make_unique<als_dimmer::CSVLogger>(csv_file);
        if (!csv_logger->isOpen()) {
            LOG_ERROR("main", "Failed to open CSV log file, continuing without CSV logging");
            csv_logger.reset();
        } else {
            LOG_INFO("main", "CSV logging enabled to: " << csv_file);
        }
    }

    // Restore initial brightness based on saved state
    if (state_mgr.getMode() == als_dimmer::OperatingMode::MANUAL) {
        output->setBrightness(state_mgr.getManualBrightness());
        LOG_INFO("main", "Restored MANUAL mode at " << state_mgr.getManualBrightness() << "%");
    } else {
        // AUTO or MANUAL_TEMPORARY -> start in AUTO
        if (state_mgr.getMode() == als_dimmer::OperatingMode::MANUAL_TEMPORARY) {
            state_mgr.setMode(als_dimmer::OperatingMode::AUTO);
            LOG_INFO("main", "MANUAL_TEMPORARY doesn't persist, starting in AUTO mode");
        }
    }

    // Main control loop
    LOG_INFO("main", "Starting control loop (update interval: " << config.control.update_interval_ms << " ms)");
    LOG_INFO("main", "TCP control available on " << config.control.listen_address << ":" << config.control.listen_port);

    auto loop_start = std::chrono::steady_clock::now();
    auto manual_temp_start = std::chrono::steady_clock::now();
    float current_lux = 0.0f;
    bool should_exit = false;

    // CSV logging state tracking
    uint64_t iteration_seq = 0;
    int previous_brightness = output->getCurrentBrightness();
    std::string previous_zone_name = "";
    auto csv_start_time = std::chrono::steady_clock::now();

    while (!should_exit) {
        // Process TCP commands
        while (control.hasCommand()) {
            std::string cmd = control.getNextCommand();
            std::string response = processCommand(cmd, state_mgr, control, current_lux,
                                                  output->getCurrentBrightness(), manual_temp_start,
                                                  zone_mapper.get());
            control.sendResponse(response);

            if (cmd == "SHUTDOWN") {
                should_exit = true;
                break;
            }
        }

        // Check for auto-resume from MANUAL_TEMPORARY
        if (state_mgr.getMode() == als_dimmer::OperatingMode::MANUAL_TEMPORARY) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - manual_temp_start).count();

            if (elapsed >= config.control.auto_resume_timeout_sec) {
                LOG_INFO("main", "Auto-resuming AUTO mode (timeout expired)");
                state_mgr.setMode(als_dimmer::OperatingMode::AUTO);
            }
        }

        // Control logic based on operating mode
        if (state_mgr.getMode() == als_dimmer::OperatingMode::AUTO) {
            // Read sensor
            current_lux = sensor->readLux();

            if (current_lux >= 0) {
                // Map lux to brightness using zone mapper (or simple mapping as fallback)
                int target_brightness;
                const als_dimmer::Zone* current_zone = nullptr;
                std::string current_zone_name;
                std::string curve_type;

                if (zone_mapper) {
                    target_brightness = zone_mapper->mapLuxToBrightness(current_lux);
                    current_zone = zone_mapper->selectZone(current_lux);
                    current_zone_name = zone_mapper->getCurrentZoneName(current_lux);
                    curve_type = current_zone ? current_zone->curve : "unknown";
                } else {
                    target_brightness = mapLuxToBrightnessSimple(current_lux);
                    current_zone_name = "simple";
                    curve_type = "linear";
                }

                // Calculate smooth brightness transition with diagnostics
                int current_brightness = output->getCurrentBrightness();
                auto transition_info = brightness_ctrl.calculateNextBrightnessWithInfo(
                    target_brightness, current_brightness, current_zone);

                // Apply brightness
                output->setBrightness(transition_info.next_brightness);
                state_mgr.setLastAutoBrightness(transition_info.next_brightness);

                // CSV logging (AUTO mode)
                if (csv_logger) {
                    auto now = std::chrono::steady_clock::now();
                    double timestamp = std::chrono::duration<double>(now - csv_start_time).count();
                    bool zone_changed = (current_zone_name != previous_zone_name);

                    als_dimmer::CSVLogger::IterationData log_data;
                    log_data.timestamp = timestamp;
                    log_data.seq = iteration_seq;
                    log_data.lux = current_lux;
                    log_data.sensor_healthy = sensor->isHealthy();
                    log_data.zone_name = current_zone_name;
                    log_data.zone_changed = zone_changed;
                    log_data.curve = curve_type;
                    log_data.target_brightness = target_brightness;
                    log_data.current_brightness = current_brightness;
                    log_data.previous_brightness = previous_brightness;
                    log_data.brightness_change = transition_info.next_brightness - previous_brightness;
                    log_data.error = transition_info.error;
                    log_data.step_category = transition_info.step_category;
                    log_data.step_size = transition_info.step_size;
                    log_data.step_threshold_large = transition_info.step_threshold_large;
                    log_data.step_threshold_small = transition_info.step_threshold_small;
                    log_data.mode = "AUTO";

                    csv_logger->logIteration(log_data);

                    previous_zone_name = current_zone_name;
                }

                previous_brightness = transition_info.next_brightness;

                // Log with zone name and transition info if available
                if (zone_mapper) {
                    LOG_TRACE("main", "AUTO: Lux=" << current_lux
                              << " Zone=" << current_zone_name
                              << " Target=" << target_brightness << "%"
                              << " Current=" << current_brightness << "%"
                              << " Next=" << transition_info.next_brightness << "%");
                } else {
                    LOG_TRACE("main", "AUTO: Lux=" << current_lux
                              << " Target=" << target_brightness << "%"
                              << " Next=" << transition_info.next_brightness << "%");
                }
            }
            iteration_seq++;
        } else {
            // MANUAL or MANUAL_TEMPORARY: use manual brightness
            int manual_brightness = state_mgr.getManualBrightness();
            int current_brightness_before = output->getCurrentBrightness();
            output->setBrightness(manual_brightness);

            std::string mode_str = als_dimmer::StateManager::modeToString(state_mgr.getMode());
            LOG_DEBUG("main", mode_str << ": Brightness=" << manual_brightness << "%");

            // CSV logging (MANUAL mode)
            if (csv_logger) {
                auto now = std::chrono::steady_clock::now();
                double timestamp = std::chrono::duration<double>(now - csv_start_time).count();

                als_dimmer::CSVLogger::IterationData log_data;
                log_data.timestamp = timestamp;
                log_data.seq = iteration_seq;
                log_data.lux = current_lux;  // May be stale in manual mode
                log_data.sensor_healthy = sensor->isHealthy();
                log_data.zone_name = "manual";
                log_data.zone_changed = false;
                log_data.curve = "manual";
                log_data.target_brightness = manual_brightness;
                log_data.current_brightness = current_brightness_before;
                log_data.previous_brightness = previous_brightness;
                log_data.brightness_change = manual_brightness - previous_brightness;
                log_data.error = 0;  // No error in manual mode
                log_data.step_category = "manual";
                log_data.step_size = 0;
                log_data.step_threshold_large = 0;
                log_data.step_threshold_small = 0;
                log_data.mode = mode_str;

                csv_logger->logIteration(log_data);
            }

            previous_brightness = manual_brightness;
            iteration_seq++;
        }

        // Periodic state save (every 60 seconds if dirty)
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - loop_start).count();
        if (uptime % 60 == 0 && state_mgr.isDirty()) {
            state_mgr.save();
        }

        // Sleep for update interval
        std::this_thread::sleep_for(std::chrono::milliseconds(config.control.update_interval_ms));
    }

    // Cleanup
    state_mgr.save();
    control.stop();

    LOG_INFO("main", "Exiting");
    return 0;
}
