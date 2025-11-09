#include "als-dimmer/json_protocol.hpp"
#include <sstream>

namespace als_dimmer {
namespace protocol {

ParsedCommand parseCommand(const std::string& json_str) {
    ParsedCommand cmd;

    // Parse JSON string
    json j = json::parse(json_str);

    // Check protocol version
    if (j.contains("version")) {
        cmd.version = j["version"].get<std::string>();
    } else {
        cmd.version = "unknown";
    }

    // Get command type
    if (!j.contains("command")) {
        cmd.type = CommandType::UNKNOWN;
        return cmd;
    }

    std::string command_str = j["command"].get<std::string>();

    // Map command string to CommandType
    if (command_str == "get_status") {
        cmd.type = CommandType::GET_STATUS;
    } else if (command_str == "set_mode") {
        cmd.type = CommandType::SET_MODE;
    } else if (command_str == "set_brightness") {
        cmd.type = CommandType::SET_BRIGHTNESS;
    } else if (command_str == "adjust_brightness") {
        cmd.type = CommandType::ADJUST_BRIGHTNESS;
    } else if (command_str == "get_config") {
        cmd.type = CommandType::GET_CONFIG;
    } else {
        cmd.type = CommandType::UNKNOWN;
    }

    // Extract parameters if present
    if (j.contains("params")) {
        cmd.params = j["params"];
    } else {
        cmd.params = json::object();
    }

    return cmd;
}

std::string generateResponse(ResponseStatus status,
                            const std::string& message,
                            const json& data) {
    json response;
    response["version"] = PROTOCOL_VERSION;
    response["status"] = responseStatusToString(status);
    response["message"] = message;

    if (!data.empty()) {
        response["data"] = data;
    }

    return response.dump();
}

std::string generateStatusResponse(const std::string& mode,
                                   int current_brightness,
                                   float current_lux,
                                   const std::string& current_zone) {
    json data;
    data["mode"] = mode;  // Now accepts: "auto", "manual", or "manual_temporary"
    data["brightness"] = current_brightness;
    data["lux"] = current_lux;
    data["zone"] = current_zone;

    return generateResponse(ResponseStatus::SUCCESS,
                          "Status retrieved successfully",
                          data);
}

std::string generateConfigResponse(const json& config_data) {
    return generateResponse(ResponseStatus::SUCCESS,
                          "Configuration retrieved successfully",
                          config_data);
}

std::string generateErrorResponse(const std::string& error_message,
                                 const std::string& error_code) {
    json data;
    if (!error_code.empty()) {
        data["error_code"] = error_code;
    }

    return generateResponse(ResponseStatus::ERROR,
                          error_message,
                          data);
}

std::string commandTypeToString(CommandType type) {
    switch (type) {
        case CommandType::GET_STATUS:
            return "get_status";
        case CommandType::SET_MODE:
            return "set_mode";
        case CommandType::SET_BRIGHTNESS:
            return "set_brightness";
        case CommandType::ADJUST_BRIGHTNESS:
            return "adjust_brightness";
        case CommandType::GET_CONFIG:
            return "get_config";
        case CommandType::UNKNOWN:
        default:
            return "unknown";
    }
}

std::string responseStatusToString(ResponseStatus status) {
    switch (status) {
        case ResponseStatus::SUCCESS:
            return "success";
        case ResponseStatus::ERROR:
            return "error";
        case ResponseStatus::INVALID_COMMAND:
            return "invalid_command";
        case ResponseStatus::INVALID_PARAMS:
            return "invalid_params";
        default:
            return "unknown";
    }
}

} // namespace protocol
} // namespace als_dimmer
