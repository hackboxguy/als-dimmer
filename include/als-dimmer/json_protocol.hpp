#ifndef ALS_DIMMER_JSON_PROTOCOL_HPP
#define ALS_DIMMER_JSON_PROTOCOL_HPP

#include <string>
#include <memory>
#include "json.hpp"

namespace als_dimmer {

using json = nlohmann::json;

// Protocol version
constexpr const char* PROTOCOL_VERSION = "1.0";

// JSON Protocol Helper Functions
namespace protocol {

// Command types
enum class CommandType {
    GET_STATUS,
    SET_MODE,
    SET_BRIGHTNESS,
    ADJUST_BRIGHTNESS,
    GET_CONFIG,
    UNKNOWN
};

// Response status
enum class ResponseStatus {
    SUCCESS,
    ERROR,
    INVALID_COMMAND,
    INVALID_PARAMS
};

// Parse incoming JSON command
// Returns: CommandType and parsed parameters as JSON object
// Throws: json::parse_error if invalid JSON
struct ParsedCommand {
    CommandType type;
    json params;
    std::string version;
};

ParsedCommand parseCommand(const std::string& json_str);

// Generate JSON response
// Returns: JSON string ready to send to client
std::string generateResponse(ResponseStatus status,
                            const std::string& message,
                            const json& data = json::object());

// Generate status response (for GET_STATUS command)
std::string generateStatusResponse(bool auto_mode,
                                   int current_brightness,
                                   float current_lux,
                                   const std::string& current_zone);

// Generate config response (for GET_CONFIG command)
std::string generateConfigResponse(const json& config_data);

// Generate error response
std::string generateErrorResponse(const std::string& error_message,
                                 const std::string& error_code = "");

// Helper to convert CommandType to string
std::string commandTypeToString(CommandType type);

// Helper to convert ResponseStatus to string
std::string responseStatusToString(ResponseStatus status);

} // namespace protocol
} // namespace als_dimmer

#endif // ALS_DIMMER_JSON_PROTOCOL_HPP
