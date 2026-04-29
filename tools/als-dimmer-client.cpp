/**
 * ALS-Dimmer Client Utility
 * Command-line client for controlling the ALS-Dimmer daemon via JSON protocol
 */

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Minimal JSON parser for simple responses (no external dependencies)
#include <map>

// Default connection settings
constexpr const char* DEFAULT_IP = "127.0.0.1";
constexpr int DEFAULT_PORT = 9000;
constexpr const char* DEFAULT_SOCKET = "/tmp/als-dimmer.sock";

// Exit codes
constexpr int EXIT_SUCCESS_CODE = 0;
constexpr int EXIT_INVALID_ARGS = 1;
constexpr int EXIT_CONNECTION_FAILED = 2;
constexpr int EXIT_SEND_FAILED = 3;
constexpr int EXIT_RECEIVE_FAILED = 4;
constexpr int EXIT_PARSE_FAILED = 5;
constexpr int EXIT_COMMAND_FAILED = 6;

enum class SocketType { TCP, UNIX };

struct ConnectionConfig {
    std::string ip = DEFAULT_IP;
    int port = DEFAULT_PORT;
    std::string socket_path = DEFAULT_SOCKET;
    SocketType socket_type = SocketType::TCP;
};

struct CommandConfig {
    enum class Type {
        NONE,
        GET_STATUS,
        GET_BRIGHTNESS,
        SET_BRIGHTNESS,
        GET_MODE,
        SET_MODE,
        ADJUST_BRIGHTNESS,
        GET_ABSOLUTE_BRIGHTNESS,
        SET_ABSOLUTE_BRIGHTNESS,
        GET_MAX_BRIGHTNESS,
        GET_MIN_BRIGHTNESS,
        GET_CALIBRATION_INFO
    };
    Type type = Type::NONE;
    int value = 0;
    double float_value = 0.0;  // for set_absolute_brightness (nits is a double)
    std::string mode;
    bool json_output = false;
};

void printUsage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] COMMAND\n\n"
              << "Connection Options:\n"
              << "  --ip=IP              Server IP address (default: " << DEFAULT_IP << ")\n"
              << "  --port=PORT          Server port (default: " << DEFAULT_PORT << ")\n"
              << "  --socket=PATH        Unix socket path (default: " << DEFAULT_SOCKET << ")\n"
              << "  --use-unix-socket    Use Unix domain socket instead of TCP\n\n"
              << "Output Options:\n"
              << "  --json               Output raw JSON response\n\n"
              << "Commands:\n"
              << "  --status                  Get daemon status (mode, brightness, lux, zone, sensor, nits)\n"
              << "  --brightness              Get current brightness (0-100)\n"
              << "  --brightness=VALUE        Set brightness to VALUE (0-100)\n"
              << "  --mode                    Get current mode (auto/manual)\n"
              << "  --mode=MODE               Set mode to MODE (auto or manual)\n"
              << "  --adjust=DELTA            Adjust brightness by DELTA (-100 to +100)\n"
              << "  --absolute-brightness     Get current brightness in nits (requires calibrated LUT)\n"
              << "  --absolute-brightness=N   Set brightness to N nits (requires calibrated LUT)\n"
              << "  --max-brightness          Print max nits supported by the loaded calibration LUT\n"
              << "  --min-brightness          Print min nits supported by the loaded calibration LUT\n"
              << "  --calibration-info        Show LUT status, range, label, output_type tag\n\n"
              << "Examples:\n"
              << "  " << program_name << " --status\n"
              << "  " << program_name << " --brightness=75\n"
              << "  " << program_name << " --mode=auto\n"
              << "  " << program_name << " --adjust=10\n"
              << "  " << program_name << " --absolute-brightness        # current nits\n"
              << "  " << program_name << " --absolute-brightness=750    # set 750 nits\n"
              << "  " << program_name << " --max-brightness             # discover panel range\n"
              << "  " << program_name << " --calibration-info\n"
              << "  " << program_name << " --ip=192.168.1.100 --port=9000 --status\n"
              << "  " << program_name << " --use-unix-socket --status\n"
              << "  " << program_name << " --status --json\n";
}

std::string parseArgValue(const char* arg, const char* /*prefix*/) {
    const char* eq = strchr(arg, '=');
    if (eq) {
        return std::string(eq + 1);
    }
    return "";
}

// Simple JSON value extractor (for basic string/number values)
std::string extractJsonValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.length()) return "";

    if (json[pos] == '"') {
        // String value
        pos++;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    } else {
        // Number or boolean
        size_t end = pos;
        while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\n') end++;
        return json.substr(pos, end - pos);
    }
}

// Build JSON request string
std::string buildJsonRequest(const std::string& command) {
    return "{\"version\":\"1.0\",\"command\":\"" + command + "\"}";
}

std::string buildJsonRequest(const std::string& command, const std::string& param_name, int param_value) {
    std::ostringstream oss;
    oss << "{\"version\":\"1.0\",\"command\":\"" << command
        << "\",\"params\":{\"" << param_name << "\":" << param_value << "}}";
    return oss.str();
}

std::string buildJsonRequest(const std::string& command, const std::string& param_name, const std::string& param_value) {
    return "{\"version\":\"1.0\",\"command\":\"" + command +
           "\",\"params\":{\"" + param_name + "\":\"" + param_value + "\"}}";
}

std::string buildJsonRequest(const std::string& command, const std::string& param_name, double param_value) {
    std::ostringstream oss;
    oss.precision(6);
    oss << "{\"version\":\"1.0\",\"command\":\"" << command
        << "\",\"params\":{\"" << param_name << "\":" << param_value << "}}";
    return oss.str();
}

bool parseArguments(int argc, char* argv[], ConnectionConfig& conn, CommandConfig& cmd) {
    if (argc < 2) {
        return false;
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg.rfind("--ip=", 0) == 0) {
            conn.ip = parseArgValue(argv[i], "--ip=");
        } else if (arg.rfind("--port=", 0) == 0) {
            conn.port = std::stoi(parseArgValue(argv[i], "--port="));
        } else if (arg.rfind("--socket=", 0) == 0) {
            conn.socket_path = parseArgValue(argv[i], "--socket=");
        } else if (arg == "--use-unix-socket") {
            conn.socket_type = SocketType::UNIX;
        } else if (arg == "--json") {
            cmd.json_output = true;
        } else if (arg == "--status") {
            cmd.type = CommandConfig::Type::GET_STATUS;
        } else if (arg == "--brightness") {
            cmd.type = CommandConfig::Type::GET_BRIGHTNESS;
        } else if (arg.rfind("--brightness=", 0) == 0) {
            cmd.type = CommandConfig::Type::SET_BRIGHTNESS;
            cmd.value = std::stoi(parseArgValue(argv[i], "--brightness="));
            if (cmd.value < 0 || cmd.value > 100) {
                std::cerr << "Error: Brightness must be between 0 and 100\n";
                return false;
            }
        } else if (arg == "--mode") {
            cmd.type = CommandConfig::Type::GET_MODE;
        } else if (arg.rfind("--mode=", 0) == 0) {
            cmd.type = CommandConfig::Type::SET_MODE;
            cmd.mode = parseArgValue(argv[i], "--mode=");
            if (cmd.mode != "auto" && cmd.mode != "manual") {
                std::cerr << "Error: Mode must be 'auto' or 'manual'\n";
                return false;
            }
        } else if (arg.rfind("--adjust=", 0) == 0) {
            cmd.type = CommandConfig::Type::ADJUST_BRIGHTNESS;
            cmd.value = std::stoi(parseArgValue(argv[i], "--adjust="));
            if (cmd.value < -100 || cmd.value > 100) {
                std::cerr << "Error: Adjust delta must be between -100 and +100\n";
                return false;
            }
        } else if (arg == "--absolute-brightness") {
            cmd.type = CommandConfig::Type::GET_ABSOLUTE_BRIGHTNESS;
        } else if (arg.rfind("--absolute-brightness=", 0) == 0) {
            cmd.type = CommandConfig::Type::SET_ABSOLUTE_BRIGHTNESS;
            try {
                cmd.float_value = std::stod(parseArgValue(argv[i], "--absolute-brightness="));
            } catch (const std::exception&) {
                std::cerr << "Error: --absolute-brightness value must be a number\n";
                return false;
            }
            if (cmd.float_value < 0.0) {
                std::cerr << "Error: --absolute-brightness value must be >= 0\n";
                return false;
            }
        } else if (arg == "--max-brightness") {
            cmd.type = CommandConfig::Type::GET_MAX_BRIGHTNESS;
        } else if (arg == "--min-brightness") {
            cmd.type = CommandConfig::Type::GET_MIN_BRIGHTNESS;
        } else if (arg == "--calibration-info") {
            cmd.type = CommandConfig::Type::GET_CALIBRATION_INFO;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else {
            std::cerr << "Error: Unknown option: " << arg << "\n\n";
            return false;
        }
    }

    if (cmd.type == CommandConfig::Type::NONE) {
        std::cerr << "Error: No command specified\n\n";
        return false;
    }

    return true;
}

int connectToServer(const ConnectionConfig& conn) {
    int sock_fd;

    if (conn.socket_type == SocketType::UNIX) {
        // Unix domain socket
        sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::cerr << "Error: Failed to create Unix socket: " << strerror(errno) << "\n";
            return -1;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, conn.socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Error: Failed to connect to Unix socket " << conn.socket_path
                      << ": " << strerror(errno) << "\n";
            close(sock_fd);
            return -1;
        }
    } else {
        // TCP socket
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::cerr << "Error: Failed to create TCP socket: " << strerror(errno) << "\n";
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(conn.port);

        if (inet_pton(AF_INET, conn.ip.c_str(), &addr.sin_addr) <= 0) {
            std::cerr << "Error: Invalid IP address: " << conn.ip << "\n";
            close(sock_fd);
            return -1;
        }

        if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Error: Failed to connect to " << conn.ip << ":" << conn.port
                      << ": " << strerror(errno) << "\n";
            close(sock_fd);
            return -1;
        }
    }

    return sock_fd;
}

std::string sendCommand(int sock_fd, const std::string& json_request) {
    // Send request
    ssize_t sent = send(sock_fd, json_request.c_str(), json_request.length(), 0);
    if (sent < 0) {
        std::cerr << "Error: Failed to send command: " << strerror(errno) << "\n";
        return "";
    }

    // Receive response
    char buffer[4096];
    ssize_t received = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
    if (received < 0) {
        std::cerr << "Error: Failed to receive response: " << strerror(errno) << "\n";
        return "";
    }

    buffer[received] = '\0';
    return std::string(buffer);
}

// Print a "value or uncalibrated" message and return whether it was uncalibrated.
// Used by --absolute-brightness, --max-brightness, --min-brightness.
// When uncalibrated: prints "(uncalibrated)" to stderr, returns true so the
// caller can exit non-zero.
bool printNumericOrUncalibrated(const std::string& json_response,
                                const std::string& json_key) {
    std::string calibrated = extractJsonValue(json_response, "calibrated");
    if (calibrated != "true") {
        std::cerr << "(uncalibrated)\n";
        return true;
    }
    std::string val = extractJsonValue(json_response, json_key);
    if (val.empty() || val == "null") {
        std::cerr << "(uncalibrated)\n";
        return true;
    }
    std::cout << val << "\n";
    return false;
}

// Returns true if the command effectively succeeded; false if the daemon
// returned status:error OR if a numeric query returned (uncalibrated). Used
// by main() to choose the process exit code.
bool printResponse(const std::string& json_response, const CommandConfig& cmd) {
    if (cmd.json_output) {
        std::cout << json_response << "\n";
        std::string status = extractJsonValue(json_response, "status");
        return status != "error";
    }

    // Check status
    std::string status = extractJsonValue(json_response, "status");
    if (status == "error") {
        std::string message = extractJsonValue(json_response, "message");
        std::cerr << "Error: " << message << "\n";
        std::string error_code = extractJsonValue(json_response, "error_code");
        if (!error_code.empty()) {
            std::cerr << "Error code: " << error_code << "\n";
        }
        return false;
    }

    // Format output based on command type
    switch (cmd.type) {
        case CommandConfig::Type::GET_STATUS: {
            std::string mode = extractJsonValue(json_response, "mode");
            std::string brightness = extractJsonValue(json_response, "brightness");
            std::string lux = extractJsonValue(json_response, "lux");
            std::string zone = extractJsonValue(json_response, "zone");
            std::string sensor_status = extractJsonValue(json_response, "sensor_status");
            std::string calibrated = extractJsonValue(json_response, "calibrated");
            std::string nits = extractJsonValue(json_response, "nits");
            std::cout << "Status:\n"
                      << "  Mode: " << mode << "\n"
                      << "  Brightness: " << brightness << "%\n";
            if (calibrated == "true" && !nits.empty() && nits != "null") {
                std::cout << "  Nits: " << nits << " (calibrated)\n";
            } else {
                std::cout << "  Nits: (uncalibrated)\n";
            }
            std::cout << "  Lux: " << lux << "\n"
                      << "  Zone: " << zone << "\n";
            if (!sensor_status.empty()) {
                std::cout << "  Sensor: " << sensor_status << "\n";
            }
            break;
        }
        case CommandConfig::Type::GET_BRIGHTNESS: {
            std::string brightness = extractJsonValue(json_response, "manual_brightness");
            std::cout << brightness << "\n";
            break;
        }
        case CommandConfig::Type::SET_BRIGHTNESS:
            std::cout << "Brightness set to " << cmd.value << "%\n";
            {
                std::string mode = extractJsonValue(json_response, "mode");
                if (!mode.empty()) {
                    std::cout << "Mode: " << mode << "\n";
                }
            }
            break;
        case CommandConfig::Type::GET_MODE: {
            std::string mode = extractJsonValue(json_response, "mode");
            std::cout << mode << "\n";
            break;
        }
        case CommandConfig::Type::SET_MODE:
            std::cout << "Mode set to " << cmd.mode << "\n";
            break;
        case CommandConfig::Type::ADJUST_BRIGHTNESS: {
            std::string brightness = extractJsonValue(json_response, "brightness");
            std::cout << "Brightness adjusted by " << (cmd.value > 0 ? "+" : "") << cmd.value << "%\n"
                      << "New brightness: " << brightness << "%\n";
            break;
        }
        case CommandConfig::Type::GET_ABSOLUTE_BRIGHTNESS: {
            if (printNumericOrUncalibrated(json_response, "nits")) return false;
            break;
        }
        case CommandConfig::Type::SET_ABSOLUTE_BRIGHTNESS: {
            std::string brightness_pct = extractJsonValue(json_response, "brightness_pct");
            std::string actual_nits = extractJsonValue(json_response, "actual_nits");
            std::string target_nits = extractJsonValue(json_response, "target_nits");
            std::string clamped = extractJsonValue(json_response, "clamped");
            std::cout << "Absolute brightness set to " << target_nits << " nits"
                      << " -> " << brightness_pct << "% (actual: " << actual_nits << " nits)";
            if (clamped == "true") {
                std::cout << " (clamped to LUT range)";
            }
            std::cout << "\n";
            break;
        }
        case CommandConfig::Type::GET_MAX_BRIGHTNESS: {
            if (printNumericOrUncalibrated(json_response, "max_nits")) return false;
            break;
        }
        case CommandConfig::Type::GET_MIN_BRIGHTNESS: {
            if (printNumericOrUncalibrated(json_response, "min_nits")) return false;
            break;
        }
        case CommandConfig::Type::GET_CALIBRATION_INFO: {
            std::string calibrated = extractJsonValue(json_response, "calibrated");
            if (calibrated != "true") {
                std::cout << "Calibrated: no\n";
                break;
            }
            std::string min_nits = extractJsonValue(json_response, "min_nits");
            std::string max_nits = extractJsonValue(json_response, "max_nits");
            std::string label = extractJsonValue(json_response, "label");
            std::string output_type = extractJsonValue(json_response, "output_type");
            std::string row_count = extractJsonValue(json_response, "row_count");
            std::cout << "Calibrated: yes\n"
                      << "  Range: " << min_nits << " .. " << max_nits << " nits\n";
            if (!label.empty()) {
                std::cout << "  Label: " << label << "\n";
            }
            if (!output_type.empty()) {
                std::cout << "  Output type: " << output_type << "\n";
            }
            if (!row_count.empty()) {
                std::cout << "  Rows: " << row_count << "\n";
            }
            break;
        }
        default:
            std::cout << json_response << "\n";
            break;
    }
    return true;
}

int main(int argc, char* argv[]) {
    ConnectionConfig conn;
    CommandConfig cmd;

    // Parse arguments
    if (!parseArguments(argc, argv, conn, cmd)) {
        printUsage(argv[0]);
        return EXIT_INVALID_ARGS;
    }

    // Build JSON request
    std::string json_request;
    switch (cmd.type) {
        case CommandConfig::Type::GET_STATUS:
            json_request = buildJsonRequest("get_status");
            break;
        case CommandConfig::Type::GET_BRIGHTNESS:
        case CommandConfig::Type::GET_MODE:
            json_request = buildJsonRequest("get_config");
            break;
        case CommandConfig::Type::SET_BRIGHTNESS:
            json_request = buildJsonRequest("set_brightness", "brightness", cmd.value);
            break;
        case CommandConfig::Type::SET_MODE:
            json_request = buildJsonRequest("set_mode", "mode", cmd.mode);
            break;
        case CommandConfig::Type::ADJUST_BRIGHTNESS:
            json_request = buildJsonRequest("adjust_brightness", "delta", cmd.value);
            break;
        case CommandConfig::Type::GET_ABSOLUTE_BRIGHTNESS:
            json_request = buildJsonRequest("get_absolute_brightness");
            break;
        case CommandConfig::Type::SET_ABSOLUTE_BRIGHTNESS:
            json_request = buildJsonRequest("set_absolute_brightness", "nits", cmd.float_value);
            break;
        case CommandConfig::Type::GET_MAX_BRIGHTNESS:
        case CommandConfig::Type::GET_MIN_BRIGHTNESS:
        case CommandConfig::Type::GET_CALIBRATION_INFO:
            json_request = buildJsonRequest("get_calibration_info");
            break;
        default:
            std::cerr << "Error: Invalid command\n";
            return EXIT_INVALID_ARGS;
    }

    // Connect to server
    int sock_fd = connectToServer(conn);
    if (sock_fd < 0) {
        return EXIT_CONNECTION_FAILED;
    }

    // Send command and receive response
    std::string json_response = sendCommand(sock_fd, json_request);
    close(sock_fd);

    if (json_response.empty()) {
        return EXIT_RECEIVE_FAILED;
    }

    // Parse and print response. printResponse returns false on either an
    // explicit daemon-side error OR an "(uncalibrated)" numeric query, both
    // of which should map to a non-zero exit so scripts can check $?.
    try {
        if (!printResponse(json_response, cmd)) {
            return EXIT_COMMAND_FAILED;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to parse response: " << e.what() << "\n";
        std::cerr << "Raw response: " << json_response << "\n";
        return EXIT_PARSE_FAILED;
    }

    return EXIT_SUCCESS_CODE;
}
