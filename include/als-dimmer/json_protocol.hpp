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
    GET_ABSOLUTE_BRIGHTNESS,
    SET_ABSOLUTE_BRIGHTNESS,
    GET_CALIBRATION_INFO,
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
// sensor_status:    "available" or "unavailable" - lets clients grey out the AUTO toggle.
// calibrated:       true if a brightness->nits LUT is loaded; when false, `nits`
//                   is omitted from the JSON ("nits": null) and the value is ignored.
// thermal_enabled:     true if a thermal-compensation factor table is loaded AND polling
//                      is active. When false, the next three fields are emitted as null.
// thermal_has_reading: true if the polling thread has produced at least one successful
//                      temp reading AND it isn't stale. Distinct from thermal_enabled -
//                      the feature can be configured (enabled=true) but not actually
//                      working (has_reading=false), e.g. because temp_command is failing.
//                      When false, backlight_temp_c and thermal_factor are null so
//                      clients can distinguish "no live data" from "data shows 1.0".
// backlight_temp_c:    most recent successful temperature reading (degC).
// thermal_factor:      the correction currently being applied to LUT-predicted nits.
std::string generateStatusResponse(const std::string& mode,
                                   int current_brightness,
                                   float current_lux,
                                   const std::string& current_zone,
                                   const std::string& sensor_status = "available",
                                   bool calibrated = false,
                                   double nits = 0.0,
                                   bool thermal_enabled = false,
                                   bool thermal_has_reading = false,
                                   double backlight_temp_c = 0.0,
                                   double thermal_factor = 1.0);

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
