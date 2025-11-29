# ALS-Dimmer: Ambient Light Sensor Based Display Brightness Control

## Project Overview

ALS-Dimmer is a C++ daemon for automatic display brightness control based on ambient light sensors (ALS). Designed primarily for automotive in-vehicle environments where ambient light changes drastically (tunnels, sunrise/sunset, parking garages).

### Key Features
- Multiple sensor input types: OPTI4001, FPGA-cached OPTI4001, VEML7700, CAN-based ALS, custom I2C sensors, file-based simulation
- Multiple output types: DDC/CI (libddcutil), custom I2C displays (dimmer200/dimmer800), CAN output, file-based simulation
- Zone-based brightness mapping with logarithmic and linear curves for optimal perception
- Smooth brightness transitions with error-based step sizing (no jarring changes)
- **Dual socket support**: TCP (development/debugging) + Unix domain socket (low-latency IPC for Android)
- **JSON-based protocol** (v1.0) for structured control commands and responses
- Three operating modes: AUTO, MANUAL, MANUAL_TEMPORARY (60s auto-resume)
- Persistent state management with JSON state file
- Runtime log level control (TRACE/DEBUG/INFO/WARN/ERROR)
- Single static binary deployment (no runtime dependencies except optional libddcutil)
- Systemd integration with security hardening

### Target Platforms
- Primary: Raspberry Pi 4 (development/testing)
- Production: Automotive Android-Linux head-units with custom displays
- Development: Ubuntu desktop with DDC/CI capable monitors

---

## Design Decisions

### Core Architecture Principles

1. **C++ with Modern CMake** (C++14, target-based CMake 3.16+)
2. **Compile-time feature selection** via CMake flags (DDC/CI support optional)
3. **Plugin-like architecture** with abstract interfaces for sensors and outputs
4. **Rate-limited brightness control** (not full PID) for smooth transitions
5. **Zero runtime dependencies** for automotive deployment (except optional libddcutil for testing)

### Control Algorithm

**Operating Modes**

ALS-Dimmer supports three operating modes to balance automatic control with user preferences:

1. **AUTO Mode (Default)**
   - Continuously samples ALS sensor
   - Calculates target brightness using zone-based mapping
   - Applies rate-limited brightness control
   - User manual adjustments trigger temporary mode switch

2. **MANUAL Mode**
   - ALS sensor sampling disabled
   - Uses user-specified brightness value
   - Brightness persisted to state file
   - Remains in manual mode until explicit mode change

3. **MANUAL_TEMPORARY Mode**
   - Triggered when user adjusts brightness while in AUTO mode
   - ALS sensor sampling disabled
   - Uses user-specified brightness value
   - Auto-resumes AUTO mode after configurable timeout (default: 60 seconds)
   - Any additional manual adjustment resets timeout
   - Explicit mode command immediately switches to AUTO or MANUAL

**Mode Transition Logic:**
```
AUTO:
  - User sends SET_BRIGHTNESS → MANUAL_TEMPORARY (start 60s timer)
  - User sends SET_MODE manual → MANUAL
  
MANUAL:
  - User sends SET_MODE auto → AUTO
  - User sends SET_BRIGHTNESS → Stay in MANUAL (update value)
  
MANUAL_TEMPORARY:
  - Timer expires (60s) → AUTO (log "Auto-resuming AUTO mode")
  - User sends SET_BRIGHTNESS → MANUAL_TEMPORARY (reset timer)
  - User sends SET_MODE auto → AUTO (cancel timer)
  - User sends SET_MODE manual → MANUAL (cancel timer, make sticky)
```

**Benefits:**
- User can make quick manual adjustments without changing system behavior permanently
- Natural return to automatic control after period of inactivity
- Explicit control available when user wants persistent manual mode
- Matches typical automotive/laptop brightness behavior

**Rate-Limited Mapping (Not Full PID)**

Rationale:
- Brightness control is not safety-critical (no harm from overshoot)
- Display changes are inherently rate-limited by human perception and DDC/CI speed
- Automotive light changes are drastic but don't require complex PID tuning

Approach:
```
target_brightness = map_lux_to_brightness(current_lux, zone)
error = target_brightness - current_brightness

if |error| > LARGE_THRESHOLD:
    step = MAX_STEP_SIZE      // Fast response (tunnel entry/exit)
elif |error| > SMALL_THRESHOLD:
    step = MEDIUM_STEP_SIZE   // Medium response
else:
    step = SMALL_STEP_SIZE    // Slow convergence for stability

new_brightness = current_brightness + sign(error) * step
```

Benefits:
- Responsive to large changes (tunnel scenarios)
- Smooth for small fluctuations (clouds, shadows)
- No PID tuning required
- Easy to understand and debug
- Natural "dead zone" prevents micro-adjustments

### Zone-Based Mapping ✅ (Implemented in Phase 2)

Different ambient light ranges require different brightness curves. The ZoneMapper automatically selects the appropriate zone based on the current lux value and applies the configured curve.

**Supported Curves:**
- **Linear:** Proportional response (ideal for indoor/normal lighting conditions)
- **Logarithmic:** Fast changes at low end, slow at high end (matches human perception, ideal for night/outdoor)

**Zone Configuration:**
```json
{
  "zones": [
    {
      "name": "night",
      "lux_range": [0, 10],
      "brightness_range": [5, 30],
      "curve": "logarithmic",
      "step_sizes": {"large": 5, "medium": 2, "small": 1},
      "error_thresholds": {"large": 20, "small": 5}
    },
    {
      "name": "indoor",
      "lux_range": [10, 500],
      "brightness_range": [30, 70],
      "curve": "linear",
      "step_sizes": {"large": 8, "medium": 3, "small": 1},
      "error_thresholds": {"large": 25, "small": 8}
    },
    {
      "name": "outdoor",
      "lux_range": [500, 100000],
      "brightness_range": [70, 100],
      "curve": "logarithmic",
      "step_sizes": {"large": 10, "medium": 4, "small": 2},
      "error_thresholds": {"large": 30, "small": 10}
    }
  ]
}
```

**How it works:**
1. Daemon reads current lux value from sensor
2. ZoneMapper selects the zone that contains the lux value (`selectZone()`)
3. Appropriate curve function is applied (`calculateLinear()` or `calculateLogarithmic()`)
4. Brightness is mapped to the zone's brightness range (e.g., 0-10 lux → 5-30%)
5. Output interface sets the calculated brightness

**Backward Compatibility:**
If no zones are configured, the system falls back to simple linear mapping (0-1000 lux → 5-100%).

### Multi-Instance Synchronization

For controlling multiple displays side-by-side:

**Master instance:**
```bash
als-dimmer --config display1.json --sync-master --port 9001
```
- Reads sensor
- Calculates brightness
- Applies to its display
- Broadcasts brightness value to TCP socket

**Slave instance:**
```bash
als-dimmer --config display2.json --sync-slave --master-host 192.168.1.100 --master-port 9001
```
- Connects to master via TCP
- Receives brightness values from master
- Applies to its display
- Does NOT read sensor

Benefits:
- Clean separation via TCP socket interface
- No shared memory complexity
- Can run independently if needed
- Network accessible for remote testing and control

---

## Configuration

### JSON Configuration Structure

```json
{
  "sensor": {
    "type": "opti4001",           // can_als | opti4001 | fpga_opti4001 | file
    "device": "/dev/i2c-1",       // For I2C sensors
    "address": "0x44",            // For I2C sensors (hex string)
    "file_path": "/tmp/lux.txt",  // For file sensor (simulation)

    // CAN sensor configuration (type: "can_als")
    "can_interface": "can0",      // CAN interface name
    "can_id": "0x0A2",            // CAN message ID (hex string)
    "timeout_ms": 5000            // Timeout for stale data detection
  },
  
  "output": {
    "type": "ddcutil",            // can | ddcutil | custom_i2c | file
    "device": "/dev/i2c-1",       // For i2c/ddcutil
    "display_number": 0,          // For ddcutil (if multiple displays)
    "address": "0x66",            // For custom_i2c (hex string)
    "brightness_reg": "0x01",     // For custom_i2c (hex string)
    "brightness_format": "uint8", // uint8 | uint16_le | uint16_be
    "value_range": [0, 255],      // Device's native brightness range (for custom_i2c)
    "internal_range": [0, 100],   // Daemon's internal range (always 0-100, used for scaling)
    "can_interface": "can0",      // For CAN output
    "can_id": "0x456",            // For CAN output (hex string)
    "can_format": {               // CAN message format
      "byte_order": "little_endian",
      "byte_offset": 0,
      "num_bytes": 1              // Brightness as single byte (0-100)
    },
    "file_path": "/tmp/brightness.txt"  // For file output (simulation)
  },
  
  "control": {
    "listen_address": "0.0.0.0",      // Bind address (0.0.0.0 for all interfaces, 127.0.0.1 for localhost only)
    "listen_port": 9000,               // TCP port for control interface
    "update_interval_ms": 500,
    "sensor_error_timeout_sec": 300,
    "fallback_brightness": 50,
    "state_file": "/var/lib/als-dimmer/state.json",  // Persistent state (mode, brightness)
    "auto_resume_timeout_sec": 60      // Auto-resume AUTO mode after manual adjustment (0 = disabled, sticky manual)
  },
  
  "zones": [
    {
      "name": "night",
      "lux_range": [0, 10],
      "brightness_range": [5, 30],
      "curve": "logarithmic",
      "step_sizes": {"large": 5, "medium": 2, "small": 1},
      "error_thresholds": {"large": 20, "small": 5}
    },
    {
      "name": "indoor",
      "lux_range": [10, 500],
      "brightness_range": [30, 70],
      "curve": "linear",
      "step_sizes": {"large": 8, "medium": 3, "small": 1},
      "error_thresholds": {"large": 25, "small": 8}
    },
    {
      "name": "outdoor",
      "lux_range": [500, 100000],
      "brightness_range": [70, 100],
      "curve": "logarithmic",
      "step_sizes": {"large": 10, "medium": 4, "small": 2},
      "error_thresholds": {"large": 30, "small": 10}
    }
  ],
  
  "calibration": {
    "enabled": false,
    "sample_duration_sec": 60,
    "auto_adjust_zones": true
  }
}
```

### Configuration Validation

**Critical fields (daemon refuses to start if missing):**
- `sensor.type`
- `sensor.device` OR `sensor.can_interface` (depending on type)
- `output.type`
- `output.device` OR `output.can_interface` (depending on type)
- At least one zone in `zones` array

**Optional fields (use defaults if missing):**
- `control.listen_address` → `127.0.0.1` (localhost only for security)
- `control.listen_port` → 9000
- `control.update_interval_ms` → 500
- `control.sensor_error_timeout_sec` → 300
- `control.fallback_brightness` → 50
- `control.state_file` → `/var/lib/als-dimmer/state.json`
- `control.auto_resume_timeout_sec` → 60 (0 = disabled, sticky manual mode)
- `calibration.*` → disabled by default
- `zones[].step_sizes.*` → reasonable defaults per zone
- `zones[].error_thresholds.*` → reasonable defaults per zone

---

## Socket Control Interface

### Dual Socket Support

The daemon provides two types of sockets for maximum flexibility:

1. **TCP Socket** (127.0.0.1:9000 by default)
   - For remote access, development, and debugging
   - Configurable via `control.tcp_socket` in config
   - Can be disabled if not needed

2. **Unix Domain Socket** (/tmp/als-dimmer.sock by default)
   - For low-latency local IPC (ideal for Android IVI integration)
   - Configurable permissions, owner, and group
   - Configurable via `control.unix_socket` in config
   - Can be disabled if not needed

Both sockets use the same JSON-based protocol and can operate concurrently.

### Protocol (JSON-Based v1.0)

All communication uses structured JSON messages for reliability and extensibility.

**Command Format (Client → Server):**
```json
{
  "version": "1.0",
  "command": "command_name",
  "params": {
    "param1": "value1",
    "param2": 123
  }
}
```

**Response Format (Server → Client):**
```json
{
  "version": "1.0",
  "status": "success",
  "message": "Operation completed successfully",
  "data": {
    "key": "value"
  }
}
```

### Supported Commands

**1. GET_STATUS** - Get current system status
```json
// Request:
{"version": "1.0", "command": "get_status"}

// Response:
{
  "version": "1.0",
  "status": "success",
  "message": "Status retrieved successfully",
  "data": {
    "mode": "auto",                    // or "manual", "manual_temporary"
    "brightness": 75,
    "lux": 450.5,
    "zone": "indoor"
  }
}
```

**Mode Values:**
- `"auto"` - Automatic brightness control based on sensor
- `"manual"` - Persistent manual brightness (survives restarts)
- `"manual_temporary"` - Temporary manual override with auto-resume timeout

**2. GET_CONFIG** - Get current configuration and state
```json
// Request:
{"version": "1.0", "command": "get_config"}

// Response:
{
  "version": "1.0",
  "status": "success",
  "message": "Configuration retrieved successfully",
  "data": {
    "mode": "auto",
    "manual_brightness": 75,
    "last_auto_brightness": 52
  }
}
```

**3. SET_MODE** - Set operating mode
```json
// Request:
{"version": "1.0", "command": "set_mode", "params": {"mode": "manual"}}

// Response:
{
  "version": "1.0",
  "status": "success",
  "message": "Mode set successfully",
  "data": {"mode": "manual"}
}
```

**Valid modes:** `"auto"`, `"manual"`

**4. SET_BRIGHTNESS** - Set brightness value
```json
// Request:
{"version": "1.0", "command": "set_brightness", "params": {"brightness": 75}}

// Response:
{
  "version": "1.0",
  "status": "success",
  "message": "Brightness set successfully",
  "data": {"brightness": 75}
}
```

**Behavior:**
- In AUTO mode: Switches to MANUAL_TEMPORARY and starts auto-resume timer
- In MANUAL mode: Updates brightness value, stays in MANUAL
- In MANUAL_TEMPORARY: Updates brightness and resets auto-resume timer

**5. ADJUST_BRIGHTNESS** - Adjust brightness by delta
```json
// Request:
{"version": "1.0", "command": "adjust_brightness", "params": {"delta": 10}}

// Response:
{
  "version": "1.0",
  "status": "success",
  "message": "Brightness adjusted successfully",
  "data": {
    "brightness": 85,
    "delta": 10
  }
}
```

**Valid delta range:** -100 to +100

### Error Responses

```json
{
  "version": "1.0",
  "status": "error",
  "message": "Missing 'brightness' parameter",
  "data": {"error_code": "INVALID_PARAMS"}
}
```

**Error Codes:**
- `PARSE_ERROR` - Invalid JSON syntax
- `INVALID_PARAMS` - Missing or invalid parameters
- `UNKNOWN_COMMAND` - Unrecognized command type
- `INVALID_FORMAT` - Non-JSON command (protocol error)
- `INTERNAL_ERROR` - Server-side error

### Security Considerations

**TCP Socket:**
- Bind to `127.0.0.1` for localhost-only access (default)
- Bind to `0.0.0.0` only in trusted networks (e.g., automotive isolated network)
- No authentication in current design (trusted network assumed)
- Use firewall rules to restrict access if needed

**Unix Socket:**
- Configurable file permissions (default: 0660)
- Configurable owner/group (requires root daemon)
- Ideal for Android IVI integration with SELinux policies

### Usage Examples

**Using TCP Socket:**
```bash
# Get status
printf '{"version":"1.0","command":"get_status"}' | nc localhost 9000

# Set brightness (triggers MANUAL_TEMPORARY in AUTO mode)
printf '{"version":"1.0","command":"set_brightness","params":{"brightness":75}}' | nc localhost 9000

# Switch to manual mode
printf '{"version":"1.0","command":"set_mode","params":{"mode":"manual"}}' | nc localhost 9000

# Adjust brightness by +10
printf '{"version":"1.0","command":"adjust_brightness","params":{"delta":10}}' | nc localhost 9000
```

**Using Unix Socket:**
```bash
# Get status (faster than TCP)
printf '{"version":"1.0","command":"get_status"}' | nc -U /tmp/als-dimmer.sock

# Get configuration
printf '{"version":"1.0","command":"get_config"}' | nc -U /tmp/als-dimmer.sock
```

**Using the Client Utility:**
```bash
# Get status (formatted output)
./als-dimmer-client --status

# Set brightness
./als-dimmer-client --brightness=75

# Switch mode
./als-dimmer-client --mode=auto

# Adjust brightness
./als-dimmer-client --adjust=10

# Use Unix socket (lower latency)
./als-dimmer-client --use-unix-socket --status

# Get raw JSON
./als-dimmer-client --status --json
```

---

## Persistent State Management

### State File Format

The daemon maintains persistent state across restarts to remember user preferences and current operating mode.

**Location:** Configurable via `control.state_file` (default: `/var/lib/als-dimmer/state.json`)

**Format:**
```json
{
  "version": 1,
  "mode": "auto",
  "manual_brightness": 75,
  "last_auto_brightness": 52,
  "brightness_offset": 0,
  "last_updated": "2025-11-05T14:30:22Z"
}
```

**Fields:**
- `version`: State file format version (for future migrations)
- `mode`: Last operating mode (`auto`, `manual`, or `manual_temporary`)
- `manual_brightness`: Last user-set brightness in manual mode (0-100)
- `last_auto_brightness`: Last calculated brightness in auto mode (for smooth resume)
- `brightness_offset`: Optional user preference offset applied in auto mode (-20 to +20)
- `last_updated`: ISO 8601 timestamp of last state update

### State Persistence Strategy

**Write triggers:**
1. **Mode changes:** Immediately write to disk
2. **Manual brightness changes:** Debounced write after 5 seconds of stability
3. **Periodic writes:** Every 60 seconds if state is dirty
4. **Daemon shutdown:** Final state write on graceful exit
5. **Explicit command:** `SAVE_STATE` command forces immediate write

**Read behavior:**
1. **At startup:** Load state file
2. **If missing/corrupted:** Use defaults (AUTO mode, 50% brightness), create new file
3. **After RELOAD_CONFIG:** Re-read state file

**Error handling:**
- State file corrupted → Log warning, use defaults, overwrite with valid state
- State file directory missing → Create directory with appropriate permissions
- Write failures → Log error, retry on next write trigger (state kept in memory)

### Startup Behavior

```
1. Read config file → Load zones, sensor settings, output settings
2. Read state file → Load last mode, last brightness
3. Initialize sensor and output
4. Set initial brightness:
   - If mode == AUTO: Use last_auto_brightness, then quickly adjust based on current lux
   - If mode == MANUAL: Use manual_brightness from state file
   - If mode == MANUAL_TEMPORARY: Convert to AUTO (temporary mode doesn't persist)
5. Start main control loop
```

### State File Permissions

For security and reliability:
```bash
# Create state directory
sudo mkdir -p /var/lib/als-dimmer
sudo chown als-dimmer:als-dimmer /var/lib/als-dimmer
sudo chmod 755 /var/lib/als-dimmer

# State file permissions (rw for daemon only)
# Automatically created with 644 permissions
```

---

## Sensor Error Handling

### Behavior

1. **Normal operation:** Sensor read succeeds, lux value used for brightness calculation
2. **Single failure:** Log error, use last valid lux value
3. **Continued failures:** Keep trying with exponential backoff
4. **Timeout exceeded:** After `sensor_error_timeout_sec` (default 300s):
   - Switch to fallback mode
   - Set brightness to `fallback_brightness` (default 50%)
   - Continue attempting sensor reads
5. **Recovery:** When sensor starts working again:
   - Log recovery
   - Resume auto mode immediately

### Exponential Backoff

```
Initial retry: 500ms (normal update interval)
After 10 failures: 1 second
After 20 failures: 2 seconds
After 40 failures: 4 seconds
Max backoff: 10 seconds
```

Prevents log spam and reduces CPU usage during prolonged sensor failures.

---

## Runtime Calibration

### Purpose

Adapt to different vehicle installations:
- Windshield tint variations
- Sensor placement differences
- Geographic location (desert vs northern climates)
- Seasonal variations

### Implementation

**Trigger methods:**
1. Command-line flag: `als-dimmer --calibrate`
2. Socket command: `CALIBRATE_START`
3. Config option: `calibration.enabled = true` (runs at startup)
4. Signal: `kill -SIGUSR1 <pid>`

**Process:**
1. Collect lux samples for `sample_duration_sec` (default 60s)
2. Calculate statistics: min, max, mean, percentiles
3. If `auto_adjust_zones = true`:
   - Adjust zone boundaries by ±20% to fit observed range
   - Suggest new brightness_range values
4. Output results to console/log
5. Optionally write suggested config to file

**Output example:**
```
Calibration Results (60 seconds):
  Min lux: 15.2
  Max lux: 8450.3
  Mean lux: 342.1
  Median lux: 280.5

Suggested zone adjustments:
  night: [0, 10] -> [0, 12] (no change needed, below observed min)
  indoor: [10, 500] -> [12, 600] (adjusted to observed range)
  outdoor: [500, 100000] -> [600, 10000] (adjusted to observed max)

Apply changes? (CALIBRATE_STOP to save, CALIBRATE_CANCEL to discard)
```

---

## Directory Structure

```
als-dimmer/
├── CMakeLists.txt
├── README.md
├── CLAUDE.md                        # This file
├── include/
│   └── als-dimmer/
│       ├── interfaces.hpp           # Sensor/Output abstract base classes
│       ├── brightness_controller.hpp
│       ├── zone_mapper.hpp
│       ├── calibrator.hpp
│       ├── control_interface.hpp
│       ├── state_manager.hpp
│       └── config.hpp
├── src/
│   ├── main.cpp
│   ├── config.cpp
│   ├── brightness_controller.cpp
│   ├── zone_mapper.cpp
│   ├── calibrator.cpp
│   ├── control_interface.cpp
│   ├── state_manager.cpp
│   ├── sensors/
│   │   ├── opti4001_sensor.cpp
│   │   ├── veml7700_sensor.cpp      # Phase 3
│   │   ├── file_sensor.cpp
│   │   ├── can_sensor.cpp           # Phase 3
│   │   └── custom_i2c_sensor.cpp    # Phase 3
│   └── outputs/
│       ├── ddcutil_output.cpp       # Conditional on USE_DDCUTIL
│       ├── i2c_direct_output.cpp
│       ├── file_output.cpp
│       ├── can_output.cpp           # Phase 3
│       └── custom_i2c_output.cpp    # Phase 3
├── third_party/
│   └── json.hpp                     # nlohmann/json single header
├── configs/
│   ├── config_opti4001_ddcutil.json
│   ├── config_custom_i2c.json
│   ├── config_can_sensors.json
│   └── config_simulation.json
├── systemd/
│   └── als-dimmer.service
└── test/
    ├── mock_sensor.hpp
    └── mock_output.hpp
```

---

## CMake Build System

### Build Options

```bash
# Option 1: Desktop/Pi testing with DDC/CI support
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Release ..
make

# Option 2: Automotive deployment (no libddcutil dependency)
cmake -DUSE_DDCUTIL=OFF -DCMAKE_BUILD_TYPE=Release ..
make

# Check dependencies
ldd als-dimmer
# Automotive build should show NO libddcutil dependency
```

### CMakeLists.txt Structure

```cmake
cmake_minimum_required(VERSION 3.16)
project(als-dimmer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(USE_DDCUTIL "Build with DDC/CI support via libddcutil" OFF)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(I2C REQUIRED i2c)

# Core sources (always compiled)
set(CORE_SOURCES
    src/main.cpp
    src/config.cpp
    src/brightness_controller.cpp
    src/zone_mapper.cpp
    src/calibrator.cpp
    src/control_interface.cpp
    src/state_manager.cpp
    src/sensors/opti4001_sensor.cpp
    src/sensors/file_sensor.cpp
    src/outputs/i2c_direct_output.cpp
    src/outputs/file_output.cpp
)

# Create executable
add_executable(als-dimmer ${CORE_SOURCES})

# Include directories
target_include_directories(als-dimmer 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Link libraries
target_link_libraries(als-dimmer PRIVATE ${I2C_LIBRARIES} pthread)

# Conditional DDC/CI support
if(USE_DDCUTIL)
    pkg_check_modules(DDCUTIL REQUIRED ddcutil)
    
    target_sources(als-dimmer PRIVATE src/outputs/ddcutil_output.cpp)
    target_compile_definitions(als-dimmer PRIVATE HAVE_DDCUTIL)
    target_link_libraries(als-dimmer PRIVATE ${DDCUTIL_LIBRARIES})
    target_include_directories(als-dimmer PRIVATE ${DDCUTIL_INCLUDE_DIRS})
    
    message(STATUS "DDC/CI support: ENABLED")
else()
    message(STATUS "DDC/CI support: DISABLED")
endif()

# Compiler warnings
target_compile_options(als-dimmer PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# Configure systemd service file with CMAKE_INSTALL_PREFIX
if(INSTALL_SYSTEMD_SERVICE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/systemd/als-dimmer.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/als-dimmer.service
        @ONLY
    )
endif()

# Install rules
install(TARGETS als-dimmer DESTINATION bin)

# Install all config files to PREFIX/etc/als-dimmer/
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/configs/*.json")
install(FILES ${CONFIG_FILES}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/etc/als-dimmer)

# Create symlink for default config
install(CODE "
    file(CREATE_LINK
        ${CONFIG_FILE}
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/etc/als-dimmer/config.json\"
        SYMBOLIC
    )
    message(STATUS \"Created symlink: config.json -> ${CONFIG_FILE}\")
")

# Install systemd service if enabled
if(INSTALL_SYSTEMD_SERVICE)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/als-dimmer.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    message(STATUS "Systemd service will be installed to ${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()
```

---

## Implementation Phases

### Phase 0: Foundation ✓ (Complete - This Document)
- [x] Architecture design
- [x] Configuration structure
- [x] Interface definitions
- [x] Build system design

### Phase 1: Basic Functionality ✅ (COMPLETE AND TESTED - 2025-11-05)

**Status:** All tasks completed and verified on Raspberry Pi 4 hardware

**Goals:**
- ✅ Single sensor type (OPTI4001) working
- ✅ Single output type (DDC/CI) working
- ✅ File-based sensor/output for simulation
- ✅ Basic rate-limited brightness control (simple linear mapping)
- ✅ Socket interface (basic commands)
- ✅ Operating modes (AUTO/MANUAL/MANUAL_TEMPORARY)
- ✅ State persistence

**Tasks:**
1. ✅ Project skeleton with CMake
2. ✅ Config parsing (JSON, basic validation)
3. ✅ `SensorInterface` and `OutputInterface` abstract classes
4. ✅ `OPTI4001Sensor` implementation - **COMPLETED 2025-11-05**
5. ✅ `DDCUtilOutput` implementation (if USE_DDCUTIL=ON) - **COMPLETED 2025-11-05**
6. ⏸ `I2CDirectOutput` implementation for custom displays - **Deferred to Phase 3**
7. ✅ `FileSensor` and `FileOutput` for simulation
8. ✅ Simple linear brightness mapping (no zones) - **COMPLETED 2025-11-05**
9. ✅ `ControlInterface` with TCP socket handling
10. ✅ `StateManager` class for persistent state file management
11. ✅ Operating mode implementation (AUTO, MANUAL, MANUAL_TEMPORARY)
12. ✅ Mode transition logic and auto-resume timer
13. ✅ Socket commands: SET_MODE, GET_MODE, SET_BRIGHTNESS, GET_STATE, SAVE_STATE
14. ✅ Main loop integration with mode awareness
15. ✅ Basic error handling and logging

**Deliverables:**
- ✅ Working daemon that reads OPT4001, controls display brightness
- ✅ Simulation mode for testing without hardware
- ✅ TCP socket interface for control and synchronization
- ✅ Operating mode support (AUTO/MANUAL/MANUAL_TEMPORARY)
- ✅ Persistent state management across restarts
- ✅ Auto-resume functionality for temporary manual adjustments

**Hardware Testing (2025-11-05):**
- ✅ Raspberry Pi 4 + OPT4001 (I2C 0x44) + DDC/CI Monitor
- ✅ Sensor readings: ~304 lux (indoor lighting), Device ID 0x121
- ✅ DDC/CI control: Brightness tracking working (lux 304 → 33%)
- ✅ All operating modes tested and verified
- ✅ TCP commands working with `-q 0` flag recommendation
- ✅ State persistence confirmed across restarts
- ✅ Auto-resume timer verified (60 seconds)

**Testing:**
```bash
# Hardware test
als-dimmer --config config_opti4001_ddcutil.json

# Simulation test
als-dimmer --config config_simulation.json

# In another terminal - test mode switching:
echo "GET_MODE" | nc localhost 9000
# Should return: auto

echo "SET_BRIGHTNESS 80" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000
# Should return: manual_temporary

sleep 65  # Wait for auto-resume timeout
echo "GET_MODE" | nc localhost 9000
# Should return: auto

# Test manual mode (sticky)
echo "SET_MODE manual" | nc localhost 9000
echo "SET_BRIGHTNESS 50" | nc localhost 9000
sleep 65
echo "GET_MODE" | nc localhost 9000
# Should still return: manual (no auto-resume)

# Test state persistence
echo "SET_MODE manual" | nc localhost 9000
echo "SET_BRIGHTNESS 75" | nc localhost 9000
sudo systemctl restart als-dimmer
sleep 2
echo "GET_MODE" | nc localhost 9000
# Should return: manual
echo "GET_BRIGHTNESS" | nc localhost 9000
# Should return: 75

# Test simulation mode with varying lux
for lux in 10 50 100 500 1000 5000; do
    echo $lux > /tmp/lux.txt
    sleep 2
    echo "Lux: $lux -> Brightness: $(cat /tmp/brightness.txt)"
done
```

### Phase 2: Zone-Based Brightness Mapping ✅ (COMPLETE AND TESTED - 2025-11-08)

**Status:** Core zone-based mapping implemented, tested in simulation mode, and validated on Raspberry Pi 4 hardware. **Production-ready!**

**Completed Goals:**
- ✅ Multi-zone support with different curves
- ✅ Zone selection logic (automatic based on lux value)
- ✅ Linear and logarithmic curve implementations
- ✅ Backward compatibility (configs without zones use simple linear mapping)
- ✅ Integration with main control loop and TCP status reporting

**Completed Tasks:**
1. ✅ `ZoneMapper` class implementation ([zone_mapper.hpp](../include/als-dimmer/zone_mapper.hpp), [zone_mapper.cpp](../src/zone_mapper.cpp))
2. ✅ Zone detection and switching logic (`selectZone()` method)
3. ✅ Curve implementations (linear, logarithmic)
4. ✅ Main loop integration with zone mapper
5. ✅ GET_STATUS command reports current zone
6. ✅ Logging enhanced to show zone name and curve type

**Deferred to Future Phases:**
- ⏸ Per-zone step sizes and error thresholds (step_sizes/error_thresholds in config, not yet used)
- ⏸ Smooth transitions during zone changes
- ⏸ `Calibrator` class implementation
- ⏸ Calibration trigger mechanisms
- ⏸ Auto-adjustment of zones based on calibration
- ⏸ Config reload without restart

**Implementation Details:**

The ZoneMapper class provides automatic zone selection and brightness calculation:

```cpp
class ZoneMapper {
public:
    explicit ZoneMapper(const std::vector<Zone>& zones);

    // Map lux value to brightness (0-100) using appropriate zone and curve
    int mapLuxToBrightness(float lux) const;

    // Get the current active zone for a given lux value
    const Zone* selectZone(float lux) const;

    // Get zone name for logging/debugging
    std::string getCurrentZoneName(float lux) const;

private:
    int calculateLinear(float lux, const Zone& zone) const;
    int calculateLogarithmic(float lux, const Zone& zone) const;

    std::vector<Zone> zones_;
};
```

**Curve Formulas:**

Linear curve (indoor zones):
```cpp
normalized = (lux - lux_min) / (lux_max - lux_min)
brightness = bright_min + normalized * (bright_max - bright_min)
```

Logarithmic curve (night/outdoor zones):
```cpp
// Using log(1 + x) to avoid log(0) issues
normalized = log(1 + (lux - lux_min)) / log(1 + (lux_max - lux_min))
brightness = bright_min + normalized * (bright_max - bright_min)
```

**Example Configuration:**
```json
{
  "zones": [
    {
      "name": "night",
      "lux_range": [0, 10],
      "brightness_range": [5, 30],
      "curve": "logarithmic"
    },
    {
      "name": "indoor",
      "lux_range": [10, 500],
      "brightness_range": [30, 70],
      "curve": "linear"
    },
    {
      "name": "outdoor",
      "lux_range": [500, 100000],
      "brightness_range": [70, 100],
      "curve": "logarithmic"
    }
  ]
}
```

**Testing Results (Simulation Mode - 2025-11-08):**
```bash
# Test results:
Night zone (5 lux):     23% brightness (expected: 5-30% range) ✅
Indoor zone (100 lux):  37% brightness (expected: 30-70% range) ✅
Outdoor zone (8000 lux): 93% brightness (expected: 70-100% range) ✅

# Console output shows zone selection:
[main] Zone mapper initialized with 3 zones
[ZoneMapper] Lux=100 Zone=indoor Curve=linear Brightness=37%
[main] AUTO: Lux=100 Zone=indoor -> Brightness=37%
```

**Hardware Testing Results (Raspberry Pi 4 - 2025-11-08):** ✅

Test hardware: Raspberry Pi 4 + OPT4001 (I2C 0x44) + DDC/CI Monitor

Zone transition testing:
```
Indoor → Night (tunnel entry):   Lux 250→0,    Brightness 49%→5%  ✅
Night → Indoor (tunnel exit):    Lux 0→248,    Brightness 5%→49%  ✅
Indoor → Outdoor (bright light):  Lux 250→1310, Brightness 49%→87% ✅
Outdoor → Indoor (normal light):  Lux 1310→250, Brightness 87%→49% ✅
```

Performance:
- Zone transitions: Instant and accurate
- Sensor: Stable readings (~245-256 lux indoor)
- DDC/CI: Smooth brightness control
- Automotive simulation: Perfect tunnel/outdoor handling
- Desktop use-case: Validated in office environment

**Status:** Production-ready for automotive and desktop deployments!

---

### Phase 2.5: Smooth Brightness Transitions ✅ (COMPLETE AND TESTED - 2025-11-08)

**Status:** Smooth transitions implemented and validated on Raspberry Pi 4 hardware. **Production-ready!**

**Goal:** Eliminate jarring brightness changes with gradual ramping using error-based step sizing.

**Implementation:**

The BrightnessController uses adaptive step sizing based on the magnitude of brightness error:

```cpp
class BrightnessController {
    int calculateNextBrightness(int target, int current, const Zone* zone);
private:
    int getStepSize(int error, const Zone* zone);
};
```

**Algorithm:**
```
error = target_brightness - current_brightness

if |error| > threshold_large:   use large_step  (fast convergence, e.g., 8-10 units)
elif |error| > threshold_small:  use medium_step (moderate, e.g., 3-4 units)
else:                            use small_step  (fine-tuning, e.g., 1-2 units)

next_brightness = current + sign(error) * step
```

**Configuration (per-zone in config.json):**
```json
{
  "name": "indoor",
  "step_sizes": {"large": 8, "medium": 3, "small": 1},
  "error_thresholds": {"large": 25, "small": 8}
}
```

**Hardware Test Results (Raspberry Pi 4 - 2025-11-08):**

Zone transition examples:
```
Indoor → Night (50% → 5%):
  50 → 45 → 40 → 35 → 30 → 25 → 23 → 21 → 19 → ... → 5
  Time: ~4 seconds (smooth ramp-down)

Night → Indoor (19% → 49%):
  19 → 27 → 30 → 33 → 36 → 39 → 42 → 49
  Time: ~3.5 seconds (responsive recovery)

Indoor → Outdoor (42% → 87%):
  42 → 52 → 62 → 66 → 70 → 74 → 78 → ... → 87
  Time: ~4 seconds (smooth transition)

Outdoor → Indoor (78% → 49%):
  78 → 70 → 67 → 64 → 61 → 58 → 55 → 54 → 53 → 52 → 51 → 50 → 49
  Time: ~6 seconds (very smooth ramp-down)
```

**Benefits:**
- No jarring brightness changes (critical for automotive safety)
- Responsive to large changes (3-6 seconds for full transition)
- Smooth fine-tuning prevents oscillation
- Zone-aware step sizing (outdoor uses larger steps for faster convergence)
- Professional feel matching commercial automotive/laptop brightness control

**Files:**
- [brightness_controller.hpp](../include/als-dimmer/brightness_controller.hpp) - Class interface
- [brightness_controller.cpp](../src/brightness_controller.cpp) - Implementation

**Future Enhancements (Optional):**
- Runtime calibration for different environments
- User-configurable transition speed preference

---

### Phase 3: Extended I/O Support (Target: Full Feature Set)

**Goals:**
- CAN sensor support
- CAN output support
- Additional I2C sensor types (VEML7700)
- Custom I2C sensor/output types

**Tasks:**
1. `CANSensor` implementation with SocketCAN
2. `CANOutput` implementation
3. CAN message parsing/formatting
4. `VEML7700Sensor` implementation
5. `CustomI2CSensor` generic implementation
6. `CustomI2COutput` generic implementation
7. Multiple sensor support (future: sensor fusion/failover)

**Deliverables:**
- Full CAN support
- Support for multiple sensor/output types
- Generic custom I2C handlers

### Phase 4: Production Hardening (Target: Deployment Ready)

**Goals:**
- Systemd integration
- Comprehensive error handling
- Production logging
- Buildroot package
- Documentation

**Tasks:**
1. Systemd service file with proper dependencies
2. sd_notify integration
3. Proper daemonization
4. Syslog integration
5. Logrotate configuration
6. Signal handling (SIGHUP, SIGTERM, SIGUSR1)
7. Buildroot package creation (.mk file)
8. User documentation (README.md)
9. Configuration examples for common scenarios
10. Performance optimization
11. Memory leak testing (valgrind)
12. Long-term stability testing

**Deliverables:**
- Production-ready systemd service
- Buildroot package
- Complete documentation
- Tested in automotive environment

---

## Interface Definitions

### SensorInterface

```cpp
class SensorInterface {
public:
    virtual ~SensorInterface() = default;
    
    // Initialize sensor (open device, configure registers, etc.)
    // Returns true on success, false on failure
    virtual bool init() = 0;
    
    // Read current lux value
    // Returns lux value on success, negative value on error
    virtual float readLux() = 0;
    
    // Check if sensor is healthy (not timed out, responding correctly)
    virtual bool isHealthy() const = 0;
    
    // Get sensor type name for logging
    virtual std::string getType() const = 0;
};
```

### OutputInterface

```cpp
class OutputInterface {
public:
    virtual ~OutputInterface() = default;
    
    // Initialize output (open device, detect display, etc.)
    // Returns true on success, false on failure
    virtual bool init() = 0;
    
    // Set brightness (0-100)
    // Returns true on success, false on failure
    virtual bool setBrightness(int brightness) = 0;
    
    // Get current brightness (0-100)
    // Returns brightness on success, negative value on error
    virtual int getCurrentBrightness() = 0;
    
    // Get output type name for logging
    virtual std::string getType() const = 0;
};
```

### StateManager

```cpp
struct PersistentState {
    int version = 1;
    std::string mode;              // "auto", "manual", "manual_temporary"
    int manual_brightness = 50;
    int last_auto_brightness = 50;
    int brightness_offset = 0;
    std::string last_updated;      // ISO 8601 timestamp
};

class StateManager {
public:
    StateManager(const std::string& state_file_path);
    
    // Load state from file
    // Returns true on success, false if file missing/corrupted (uses defaults)
    bool load();
    
    // Save state to file
    // Returns true on success, false on write failure
    bool save(const PersistentState& state);
    
    // Get current state
    PersistentState getState() const;
    
    // Mark state as dirty (needs save)
    void markDirty();
    
    // Check if state needs saving
    bool isDirty() const;
    
private:
    std::string file_path_;
    PersistentState state_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_save_time_;
};
```

---

## OPTI4001 Sensor Implementation Notes

### Implementation Status: ✅ COMPLETED (2025-11-05)

Successfully implemented and validated on Raspberry Pi 4 with OPT4001 sensor at I2C address 0x44.

**Test Results:**
- Device ID correctly read: 0x121
- Lux readings: ~600 lux (typical indoor lighting)
- Dynamic response verified: 5-609 lux range observed
- Auto-ranging working: exponent=2 for indoor light levels
- Counter incrementing correctly: 0x0 → 0x1 → 0x2 → 0x3...

**Implementation validated against Linux kernel driver:**
https://codebrowser.dev/linux/linux/drivers/iio/light/opt4001.c.html

### I2C Interface
- Default I2C address: 0x44 (if ADDR pin is low) or 0x45 (if ADDR pin is high)
- Operates at standard (100 kHz) or fast (400 kHz) I2C speeds

### Key Registers (Correct Addresses)
- **0x00**: EXPONENT[15:12] + RESULT_MSB[11:0] (data register, MSB)
- **0x01**: RESULT_LSB[15:8] + COUNTER[7:4] + CRC[3:0] (data register, LSB)
- **0x0A**: Configuration register (NOT 0x01!)
- **0x11**: Device ID register (reads 0x121 for OPT4001)

### Initialization Sequence
1. Open I2C device and set slave address
2. Read device ID from register 0x11 to verify communication (should be 0x121)
3. Write configuration to register 0x0A:
   - Auto-range mode (RANGE = 0xC, bits[13:10])
   - 100ms conversion time (CONVERSION_TIME = 0x8, bits[9:6])
   - Continuous mode (OPERATING_MODE = 0x3, bits[5:4])
   - Configuration value: 0xC830
4. Wait for first conversion (150ms)

### Reading Lux (Correct Implementation)
1. Read register 0x00 (16-bit, MSB first): EXPONENT[15:12] + RESULT_MSB[11:0]
2. Read register 0x01 (16-bit, MSB first): RESULT_LSB[15:8] + COUNTER[7:4] + CRC[3:0]
3. Extract bit fields:
   - EXPONENT = (reg0 >> 12) & 0x0F (4 bits)
   - RESULT_MSB = reg0 & 0x0FFF (12 bits)
   - RESULT_LSB = (reg1 >> 8) & 0xFF (8 bits)
   - COUNTER = (reg1 >> 4) & 0x0F (4 bits)
   - CRC = reg1 & 0x0F (4 bits, optional validation)
4. Calculate 20-bit MANTISSA:
   - MANTISSA = (RESULT_MSB << 8) | RESULT_LSB
5. Calculate ADC_CODES:
   - ADC_CODES = MANTISSA << EXPONENT
6. Calculate lux (PicoStar variant):
   - lux = ADC_CODES * 312.5e-6
   - Formula matches Linux kernel driver: mul=3125, div=10000000

### Semi-Logarithmic Output Format
The OPT4001 uses a semi-logarithmic format for wide dynamic range (0.01 to >100,000 lux):
- 20-bit MANTISSA (bits[19:0])
- 4-bit EXPONENT (bits[3:0])
- 9 binary logarithmic full-scale ranges (auto-ranging)
- Auto-ranging eliminates need for manual range selection

### Error Handling
- I2C communication failures
- Sensor not responding (wrong address, not connected)
- Invalid device ID (not 0x121)
- Saturated readings (lux > 120,000)
- Invalid data (sensor malfunction)

**Reference:** TI OPT4001 datasheet (docs/opt4001.pdf)

---

## DDC/CI Output Implementation Notes (libddcutil)

### Initialization
```cpp
#include <ddcutil_c_api.h>

DDCA_Status rc;
DDCA_Display_Info_List* dlist = nullptr;

// Get list of displays
rc = ddca_get_display_info_list2(false, &dlist);

// Open specific display (by number)
DDCA_Display_Handle dh = nullptr;
rc = ddca_open_display2(dlist->info[display_number].dref, false, &dh);
```

### Setting Brightness
```cpp
// VCP feature code 0x10 is brightness (0-100)
DDCA_Vcp_Feature_Code feature_code = 0x10;
int brightness_value = 75;  // 0-100

rc = ddca_set_non_table_vcp_value(dh, feature_code, 0, brightness_value);
```

### Getting Current Brightness
```cpp
DDCA_Non_Table_Vcp_Value valrec;
rc = ddca_get_non_table_vcp_value(dh, 0x10, &valrec);
int current_brightness = valrec.sl;  // Current value
int max_brightness = valrec.mh;      // Max value (usually 100)
```

### Performance Considerations
- DDC/CI is SLOW (100-200ms per command)
- Use `--noverify` flag equivalent to skip readback
- Rate-limit brightness changes (don't update faster than once per 500ms)
- Consider caching current brightness to avoid unnecessary writes

---

## Custom I2C Display Control

For automotive displays with custom I2C register interface (no DDC/CI):

### Example: Simple Register Write
```cpp
// Display at I2C address 0x66
// Brightness register at 0x01
// Internal range: 0-100 (daemon works in this range)
// Device range: 0-255 (as specified in config)

int i2c_fd = open("/dev/i2c-1", O_RDWR);
ioctl(i2c_fd, I2C_SLAVE, 0x66);

// Scale from internal 0-100 to device 0-255
// brightness is in range 0-100 (internal representation)
// value_range from config: [0, 255]
uint8_t brightness_scaled = (brightness * 255) / 100;

uint8_t buf[2] = { 0x01, brightness_scaled };  // Register, Value
write(i2c_fd, buf, 2);
```

**Config example for this display:**
```json
{
  "output": {
    "type": "custom_i2c",
    "device": "/dev/i2c-1",
    "address": "0x66",
    "brightness_reg": "0x01",
    "brightness_format": "uint8",
    "value_range": [0, 255],        // Device's native range
    "internal_range": [0, 100]      // Daemon's internal range (always 0-100)
  }
}
```

**Benefits:**
- Daemon works consistently with 0-100 internally
- Each output type scales appropriately to its native range
- DDC/CI: 0-100 (no scaling needed)
- 8-bit display: 0-255
- 12-bit display: 0-4095
- PWM duty cycle: 0-1000 (for 0.1% resolution)


### Example: Multi-Register Sequence
Some displays may require:
1. Unlock register write
2. Write brightness value
3. Commit/update command

This should be configurable per custom display type.

---

## Systemd Integration

### Service File (`/lib/systemd/system/als-dimmer.service`)

```ini
[Unit]
Description=ALS-Dimmer: Ambient Light Sensor Based Display Brightness Control
After=network.target multi-user.target
Wants=network.target

[Service]
Type=notify
ExecStart=/usr/bin/als-dimmer --config /etc/als-dimmer/config.json
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/tmp /var/log/als-dimmer /var/lib/als-dimmer

# Resource limits
LimitNOFILE=1024
MemoryMax=50M

[Install]
WantedBy=multi-user.target
```

**Note:** The `ReadWritePaths` includes `/var/lib/als-dimmer` for persistent state file access.

### Usage
```bash
# Enable and start
sudo systemctl enable als-dimmer
sudo systemctl start als-dimmer

# Check status
sudo systemctl status als-dimmer

# View logs
sudo journalctl -u als-dimmer -f

# Reload config
sudo systemctl reload als-dimmer

# Stop
sudo systemctl stop als-dimmer
```

---

## Logging Strategy

### Log Levels
- **ERROR**: Critical failures (sensor init failed, config invalid)
- **WARN**: Recoverable issues (sensor timeout, using fallback)
- **INFO**: Normal operation (startup, zone changes, brightness updates)
- **DEBUG**: Detailed information (every sensor read, PID calculations)

### Log Destinations
- **Console/stdout**: During development and `--foreground` mode
- **Syslog**: For systemd service deployment
- **File**: Optional CSV logging for tuning/analysis

### Log Format
```
[2025-11-05 14:23:45.123] [INFO] [main] ALS-Dimmer v1.0.0 starting
[2025-11-05 14:23:45.150] [INFO] [state] Loaded state from /var/lib/als-dimmer/state.json: mode=auto
[2025-11-05 14:23:45.234] [INFO] [sensor] OPTI4001 initialized at 0x44
[2025-11-05 14:23:45.345] [INFO] [output] DDC/CI display opened successfully
[2025-11-05 14:23:45.456] [INFO] [control] TCP socket listening on 0.0.0.0:9000
[2025-11-05 14:23:50.567] [DEBUG] [control] Lux=325.4 Zone=indoor Target=55 Current=45 New=48
[2025-11-05 14:25:10.123] [INFO] [control] Mode changed: AUTO -> MANUAL_TEMPORARY (user adjustment)
[2025-11-05 14:25:10.124] [INFO] [control] Manual brightness set to 80
[2025-11-05 14:26:10.125] [INFO] [control] Mode changed: MANUAL_TEMPORARY -> AUTO (timeout expired)
[2025-11-05 14:27:30.456] [INFO] [state] State saved to /var/lib/als-dimmer/state.json
```

### CSV Logging (Optional)
For tuning and analysis:
```
timestamp,lux,zone,target_brightness,current_brightness,new_brightness,error,step_size
1699200000.123,325.4,indoor,55,45,48,10,3
1699200000.623,328.1,indoor,55,48,51,7,3
```

Enable via config:
```json
{
  "logging": {
    "csv_enabled": true,
    "csv_path": "/var/log/als-dimmer/data.csv"
  }
}
```

---

## Testing Strategy

### Unit Testing (Future Enhancement)
- Mock sensor/output classes
- Test zone mapper logic
- Test brightness controller calculations
- Test config validation

### Integration Testing

**Simulation mode:**
```bash
# Terminal 1: Start daemon
als-dimmer --config config_simulation.json

# Terminal 2: Drive sensor input
for lux in 5 10 50 100 200 500 1000 2000 5000 10000; do
    echo $lux > /tmp/lux.txt
    sleep 1
    cat /tmp/brightness.txt
done
```

**Hardware loop testing:**
```bash
# Cover sensor with hand (should dim)
# Shine flashlight on sensor (should brighten)
# Monitor logs for smooth transitions
journalctl -u als-dimmer -f
```

### Performance Testing
- Measure update latency (sensor read → brightness set)
- Memory usage over 24+ hours
- CPU usage (should be minimal)
- Check for memory leaks with valgrind

### Automotive Environment Testing
- Tunnel entry/exit scenarios
- Sunrise/sunset
- Parking garag(rapid light changes)
- Night driving (low light stability)
- Verify no annoying flicker or oscillation

---

## Known Limitations & Future Enhancements

### Current Limitations
1. Single sensor only (no sensor fusion or failover)
2. Simple rate-limiting (not full PID)
3. Zone transitions are step-based (not fully smooth curves)
4. No power management (doesn't detect display sleep)
5. No authentication on TCP socket interface (assumes trusted network)

### Future Enhancements (Post-Phase 4)
1. **Sensor fusion:** Average multiple sensors, detect outliers
2. **Adaptive algorithms:** Learn user preferences over time
3. **Power management:** Detect display off state, suspend operation
4. **Screen content awareness:** Adjust brightness based on displayed content (requires frame buffer access)
5. **Time-based profiles:** Different zone settings for day/night
6. **User override learning:** Remember when user manually adjusts brightness
7. **Authentication:** Token-based or certificate-based authentication for TCP socket
8. **TLS encryption:** Secure TCP connections for untrusted networks
9. **Web interface:** Configuration and monitoring via HTTP
10. **Multi-display coordination:** Master controls multiple slaves with individual offsets

---

## Dependencies

### Required (Always)
- C++14 compiler (g++ 5+ or clang++ 3.4+)
- CMake 3.16+
- libi2c-dev
- pthread
- nlohmann/json (header-only, vendored)

### Optional (Compile-Time)
- libddcutil-dev (only if USE_DDCUTIL=ON)
- libsocketcan (for CAN support in Phase 3)

### Runtime (Automotive Deployment)
- Linux kernel with I2C support
- /dev/i2c-* devices accessible
- If using CAN: SocketCAN configured

---

## Buildroot Package (Phase 4)

### Package Structure
```
package/als-dimmer/
├── als-dimmer.mk
├── Config.in
└── S90als-dimmer       # Init script for non-systemd systems
```

### als-dimmer.mk (Sketch)
```makefile
ALS_DIMMER_VERSION = 1.0.0
ALS_DIMMER_SITE = $(call github,yourrepo,als-dimmer,v$(ALS_DIMMER_VERSION))
ALS_DIMMER_LICENSE = MIT
ALS_DIMMER_LINSE_FILES = LICENSE
ALS_DIMMER_DEPENDENCIES = host-pkgconf

# Enable DDC/CI only if requested
ifeq ($(BR2_PACKAGE_ALS_DIMMER_DDCUTIL),y)
ALS_DIMMER_DEPENDENCIES += ddcutil
ALS_DIMMER_CONF_OPTS += -DUSE_DDCUTIL=ON
else
ALS_DIMMER_CONF_OPTS += -DUSE_DDCUTIL=OFF
endif

$(eval $(cmake-package))
```

---

## Command-Line Interface

### Usage
```
als-dimmer [OPTIONS]

OPTIONS:
  --config <path>           Path to JSON config file (default: /etc/als-dimmer/config.json)
  --foreground              Don't daemonize, log to console
  --calibrate               Run calibration mode for <duration> seconds
  --sync-master             Act as master, broadcast brightness via TCP
  --port <port>             TCP port for control/sync (overrides config, default: 9000)
  --sync-slave              Act as slave, listen for brightness via TCP
  --master-host <host>      Master hostname/IP when acting as slave
  --master-port <port>      Master TCP port when acting as slave (default: 9001)
  --verbose                 Enable debug logging
  --version                 Show version information
  --help                    Show this help message

EXAMPLES:
  # Normal operation
  als-dimmer --config /etc/als-dimmer/config.json
  
  # Foreground mode for debugging
  als-dimmer --config config.json --foreground --verbose
  
  # Calibration mode
  als-dimmer --config config.json --calibrate
  
  # Master-slave setup
  # Master: reads sensor, controls display1, broadcasts on TCP port 9001
  als-dimmer --config display1.json --sync-master --port 9001
  
  # Slave: receives brightness from master, controls display2
  als-dimmer --config display2.json --sync-slave --master-host 192.168.1.100 --master-port 9001
  
  # Remote control from network client
  echo "GET_STATUS" | nc 192.168.1.100 9000
```

---

## Development Workflow

### Initial Setup
```bash
# Clone repository
git clone <repository>
cd als-dimmer

# Create build directory
mkdir build && cd build

# Configure (with DDC/CI for desktop testing)
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Debug ..

# Build
make -j$(nproc)

# Run simulation test
./als-dimmer --config ../configs/config_simulation.json --foreground --verbose
```

### Iterative Development
```bash
# After code changes
make -j$(nproc)

# Quick test
./als-dimmer --config test_config.json --foreground

# Check for memory leaks
valgrind --leak-check=full ./als-dimmer --config test_config.json
```

### Deployment to Pi/Target
```bash
# Build for target
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Copy to Pi
scp als-dimmer pi@raspberrypi:/home/pi/
scp ../configs/config_opti4001_ddcutil.json pi@raspberrypi:/home/pi/config.json

# Run on Pi
ssh pi@raspberrypi
./als-dimmer --config config.json --foreground --verbose
```

---

## Contact & Contribution

This project is developed by an embedded systems engineer with 25 years of experience in automotive displays and FPGA development.

**Development Style:**
- Command-line focused, minimal GUI
- Automation and scripting preferred
- Clean, maintainable C++ code
- Comprehensive documentation
- Test-driven development where practical

**Questions or Issues:**
Document design decisions and implementation questions as you proceed through phases.

---

## Appendix: Useful References

### Datasheets
- TI OPTI4001: Ambient Light Sensor with I2C interface
- TI VEML7700: High accuracy ambient light sensor
- DDC/CI Specification: VESA Display Data Channel Command Interface

### Linux Resources
- I2C kernel documentation: `/usr/src/linux/Documentation/i2c/`
- SocketCAN documentation: `https://www.kernel.org/doc/html/latest/networking/can.html`
- libddcutil documentation: `https://www.ddcutil.com/`

### Build Systems
- Modern CMake practices: `https://cliutils.gitlab.io/modern-cmake/`
- Buildroot manual: `https://buildroot.org/downloads/manual/manual.html`

---

---

## Logging System

### Log Levels and Format

**Status:** ✅ IMPLEMENTED (2025-11-08)

The daemon uses a custom logger with 5 log levels and runtime filtering:

**Log Levels:**
- `TRACE` (0): Very detailed debugging (e.g., every register read/write)
- `DEBUG` (1): Component-level detailed information
- `INFO` (2): Normal operation events (startup, mode changes, zone transitions)
- `WARN` (3): Recoverable issues (sensor timeout, fallback mode)
- `ERROR` (4): Critical failures (init failed, I/O errors)

**Log Format:**
```
[YYYY-MM-DD HH:MM:SS] [LEVEL] [Component] Message
```

**Example Output:**
```
[2025-11-08 22:22:33] [INFO ] [main] ALS-Dimmer starting (log level: info)
[2025-11-08 22:22:33] [DEBUG] [StateManager] State loaded: mode=auto, manual_brightness=50
[2025-11-08 22:22:33] [DEBUG] [FileSensor] Initializing with file: /tmp/als-test.lux
[2025-11-08 22:22:33] [DEBUG] [ZoneMapper] Lux=250 Zone=indoor Curve=linear Brightness=49%
```

**Configuration:**
```json
{
  "control": {
    "log_level": "info"     // trace | debug | info | warn | error
  }
}
```

**Command-line override:**
```bash
# Override config log level
./als-dimmer --config config.json --log-level debug

# Available levels: trace, debug, info, warn, error
```

**Benefits:**
- Clean, quiet operation at INFO level (production use)
- Detailed diagnostics available at DEBUG/TRACE level (troubleshooting)
- Thread-safe logging with mutex protection
- Consistent timestamp format across all components
- Runtime log level filtering (no recompilation needed)

**Components Using New Logger:**
- main.cpp (control loop)
- StateManager (persistent state)
- FileSensor (file-based sensor)
- FileOutput (file-based output)
- ZoneMapper (zone selection and brightness calculation)
- ControlInterface (TCP control socket)

**Components Still Using Old Format:**
- OPTI4001Sensor (hardware I2C sensor) - uses std::cout/cerr
- I2CDimmerOutput (custom dimmer output) - uses std::cout/cerr

These still work correctly but use the old `[ComponentName] message` format without timestamps.

**Implementation:**
- Header: [include/als-dimmer/logger.hpp](../include/als-dimmer/logger.hpp)
- Macro-based API for zero-overhead when disabled
- Stream-based formatting: `LOG_DEBUG("Component", "value=" << variable)`

---

## FPGA-Based OPT4001 Sensor (fpga_opti4001)

**Status:** ✅ IMPLEMENTED (2025-11-08)

Support for FPGA-based ambient light sensor bridge where an FPGA acts as an I2C slave to the Raspberry Pi and maintains a cached reading from an OPT4001 sensor.

### Architecture

**FPGA Bridge Design:**
- **FPGA Role:** I2C slave to Raspberry Pi (configurable address, default 0x1D)
- **FPGA to Sensor:** I2C master to OPT4001 ambient light sensor
- **Caching:** FPGA maintains latest lux reading in internal cache
- **Benefit:** Reduces Pi's I2C complexity, offloads sensor timing to FPGA

### I2C Protocol

**Command Transaction:**
```
Write: 4 bytes fixed command
  0x00 0x00 0x00 0x0C

Read: 4 bytes response
  Byte 0: Reserved (ignore)
  Bytes 1-3: 24-bit lux value (big-endian)
```

**Data Extraction:**
```cpp
uint32_t raw_value = (buf[1] << 16) | (buf[2] << 8) | buf[3];
float lux = raw_value * 0.64f;
```

**Error Detection:**
- Response `0xFFFFFFFF` indicates FPGA or sensor failure
- Valid range: 0 to ~100,000 lux (OPT4001 max ~83k lux)

### Implementation Details

**Files:**
- Implementation: [src/sensors/fpga_opti4001_sensor.cpp](../src/sensors/fpga_opti4001_sensor.cpp)
- Integration: [src/main.cpp](../src/main.cpp) (factory function)
- Configuration validation: [src/config.cpp](../src/config.cpp)

**Features:**
- Direct I2C communication using Linux I2C device interface
- Big-endian 24-bit value extraction
- Error handling for I2C failures and FPGA error condition
- Debug output for first 10 readings
- Sanity check for out-of-range values (> 100k lux)

**Configuration Example:**
```json
{
  "sensor": {
    "type": "fpga_opti4001",
    "device": "/dev/i2c-1",
    "address": "0x1D"
  },
  "output": {
    "type": "ddcutil",
    "display_number": 0
  },
  "control": {
    "update_interval_ms": 500,
    "log_level": "info"
  },
  "zones": [
    {
      "name": "night",
      "lux_range": [0, 10],
      "brightness_range": [5, 30],
      "curve": "logarithmic"
    }
  ]
}
```

**Example Protocol Transaction:**
```bash
# I2C command (address 0x1D)
i2ctransfer -y 1 w4@0x1D 0x00 0x00 0x00 0x0C r4

# Example response
0x00 0x00 0x72 0xF9

# Conversion
Raw value: 0x0072F9 = 29,433
Lux: 29,433 * 0.64 = 18,837 lux
```

**Usage:**
```bash
# With DDC/CI output
./als-dimmer --config configs/config_fpga_opti4001_ddcutil.json

# With custom dimmer output
./als-dimmer --config configs/config_fpga_opti4001_dimmer200.json
```

**Test Files:**
- [configs/config_fpga_opti4001_ddcutil.json](../configs/config_fpga_opti4001_ddcutil.json)
- [configs/config_fpga_opti4001_dimmer200.json](../configs/config_fpga_opti4001_dimmer200.json)

**Benefits:**
- Simplified Pi software (single I2C transaction per reading)
- FPGA handles OPT4001 sensor timing and initialization
- Fast readings (< 5ms per transaction)
- Cached data ensures consistent readings
- Suitable for automotive deployments with FPGA-based ECUs

---

## CAN ALS Sensor (can_als)

**Status:** ✅ IMPLEMENTED (2025-11-09)

Linux SocketCAN-based ambient light sensor that receives lux data via CAN bus messages from ESP32 VEML7700 sensor.

### CAN Message Protocol

**Message Format** (8 bytes, ID 0x0A2):
```
Byte 0:   Lux value (bits 0-7)     - Low byte
Byte 1:   Lux value (bits 8-15)    - Middle byte
Byte 2:   Lux value (bits 16-23)   - High byte
Byte 3:   Status (0x00=OK, 0x01=Error)
Byte 4:   Sequence counter
Byte 5:   Config index (0-20)
Bytes 6-7: 16-bit checksum (little-endian, sum of bytes 0-5)
```

**Lux Value Encoding:**
- 3-byte little-endian unsigned integer
- Range: 0 to 16,777,215 lux
- Extraction: `lux = byte0 | (byte1 << 8) | (byte2 << 16)`

### Implementation Details

**Files:**
- Header: [include/als-dimmer/sensors/can_als_sensor.hpp](../include/als-dimmer/sensors/can_als_sensor.hpp)
- Implementation: [src/sensors/can_als_sensor.cpp](../src/sensors/can_als_sensor.cpp)
- Integration: [src/main.cpp](../src/main.cpp) (factory function)
- Configuration: [src/config.cpp](../src/config.cpp)

**Features:**
- Linux SocketCAN integration (compatible with CANable USB-to-CAN dongles)
- Non-blocking socket with CAN ID filtering
- Checksum validation for message integrity
- Stale data detection with configurable timeout
- Thread-safe cached lux value using std::atomic
- Status byte validation (rejects error messages)
- Sanity check for unrealistic values (> 200,000 lux)

**Configuration Example:**
```json
{
  "sensor": {
    "type": "can_als",
    "can_interface": "can0",
    "can_id": "0x0A2",
    "timeout_ms": 5000
  },
  "output": {
    "type": "file",
    "file_path": "/tmp/brightness_output.txt"
  },
  "control": {
    "update_interval_ms": 500,
    "log_level": "info"
  }
}
```

**CAN Interface Setup:**
```bash
# Bring up CAN interface (500 kbps bitrate)
sudo ip link set up can0 type can bitrate 500000

# Monitor CAN messages
candump can0

# Example CAN message (ID 0x0A2):
# can0  0A2   [8]  F9 72 00 00 15 03 85 01
# Decoded: Lux = 0x0072F9 = 29,433 lux
```

**Error Handling:**
1. **Interface not available**: Log error, return -1.0
2. **Stale data (timeout)**: Log warning, return last valid value (or -1.0 if never received)
3. **Invalid checksum**: Log warning, discard message, wait for next valid message
4. **Status error (0x01)**: Log warning, return -1.0
5. **Unrealistic lux (>200k)**: Log warning but accept value

**Usage:**
```bash
# With file output (testing)
./als-dimmer --config configs/config_can_als_file.json --foreground

# With DDC/CI output
./als-dimmer --config configs/config_can_als_ddcutil.json --foreground

# With debug logging to see CAN messages
./als-dimmer --config configs/config_can_als_file.json --foreground --log-level debug
```

**Test Files:**
- [configs/config_can_als_file.json](../configs/config_can_als_file.json)

**Benefits:**
- Compatible with ESP32-based CAN senders
- Works with CANable USB-to-CAN dongles (no custom hardware)
- Automotive-grade CAN bus integration
- Supports remote ALS placement (sensor can be away from display)
- Message integrity via checksum validation
- Configurable timeout for failover scenarios

**Hardware Requirements:**
- CAN interface (e.g., CANable USB-to-CAN adapter, built-in automotive CAN)
- Linux kernel with SocketCAN support
- ESP32 or other CAN sender transmitting VEML7700 lux data on ID 0x0A2

---

## Custom I2C Dimmer Output (dimmer200/dimmer800)

**Status:** ✅ IMPLEMENTED (2025-11-08)

Support for custom I2C dimmer displays with native brightness ranges of 0-200 or 0-800.

### Dimmer Types

**dimmer200:**
- Native range: 0-200
- I2C register: 0x50 (brightness)
- Typical I2C address: 0x1D
- Use case: Basic automotive displays

**dimmer800:**
- Native range: 0-800
- I2C register: 0x50 (brightness)
- Typical I2C address: 0x1D
- Use case: High-resolution automotive displays

### Implementation Details

**Files:**
- Header: [include/als-dimmer/outputs/i2c_dimmer_output.hpp](../include/als-dimmer/outputs/i2c_dimmer_output.hpp)
- Implementation: [src/outputs/i2c_dimmer_output.cpp](../src/outputs/i2c_dimmer_output.cpp)

**Features:**
- Automatic scaling from daemon's 0-100% to native device range
- Direct I2C register writes (no DDC/CI overhead)
- Fast brightness control (< 10ms per update)
- Error handling for I2C communication failures

**Configuration Examples:**

dimmer200 configuration:
```json
{
  "output": {
    "type": "dimmer200",
    "device": "/dev/i2c-1",
    "address": "0x1D"
  }
}
```

dimmer800 configuration:
```json
{
  "output": {
    "type": "dimmer800",
    "device": "/dev/i2c-1",
    "address": "0x1D"
  }
}
```

**Scaling Examples:**
```
dimmer200:
  Daemon 0% → Device 0
  Daemon 50% → Device 100
  Daemon 100% → Device 200

dimmer800:
  Daemon 0% → Device 0
  Daemon 50% → Device 400
  Daemon 100% → Device 800
```

**Usage:**
```bash
# dimmer200 configuration
./als-dimmer --config configs/config_opti4001_dimmer200.json

# dimmer800 configuration
./als-dimmer --config configs/config_opti4001_dimmer800.json
```

**Test Files:**
- [configs/config_opti4001_dimmer200.json](../configs/config_opti4001_dimmer200.json)
- [configs/config_opti4001_dimmer800.json](../configs/config_opti4001_dimmer800.json)

---

## Systemd Service Installation

**Status:** ✅ IMPLEMENTED (2025-11-08)

Flexible systemd service installation with CMake-based configuration file selection and PREFIX support.

### CMake Build Options

**Installation Options:**
```bash
# Build options
-DUSE_DDCUTIL=ON                     # Enable DDC/CI support (default: OFF)
-DINSTALL_SYSTEMD_SERVICE=ON         # Install systemd service file (default: OFF)
-DCONFIG_FILE=config_name.json       # Default config file (default: config_opti4001_ddcutil.json)
-DCMAKE_INSTALL_PREFIX=/usr/local    # Installation prefix (default: /usr/local)
```

**Example Builds:**
```bash
# Standard installation with systemd
mkdir build && cd build
cmake .. \
  -DUSE_DDCUTIL=ON \
  -DINSTALL_SYSTEMD_SERVICE=ON \
  -DCMAKE_INSTALL_PREFIX=/usr/local
make
sudo make install

# Custom config default with FPGA sensor
cmake .. \
  -DINSTALL_SYSTEMD_SERVICE=ON \
  -DCONFIG_FILE=config_fpga_opti4001_ddcutil.json \
  -DCMAKE_INSTALL_PREFIX=/opt/als-dimmer
make
sudo make install

# Buildroot/embedded system installation
cmake .. \
  -DINSTALL_SYSTEMD_SERVICE=ON \
  -DCONFIG_FILE=config_fpga_opti4001_dimmer200.json \
  -DCMAKE_INSTALL_PREFIX=/usr
make
make install DESTDIR=/path/to/target/rootfs
```

### Installation Layout

**Installed Files:**
```
${CMAKE_INSTALL_PREFIX}/
├── bin/
│   └── als-dimmer                           # Main executable
├── etc/
│   └── als-dimmer/
│       ├── config.json -> config_opti4001_ddcutil.json  # Symlink to default
│       ├── config_fpga_opti4001_ddcutil.json
│       ├── config_fpga_opti4001_dimmer200.json
│       ├── config_opti4001_ddcutil.json
│       ├── config_opti4001_dimmer200.json
│       ├── config_opti4001_dimmer800.json
│       └── config_simulation.json
└── lib/
    └── systemd/
        └── system/
            └── als-dimmer.service           # Only if INSTALL_SYSTEMD_SERVICE=ON
```

**Config Symlink Behavior:**
- All config files are installed as examples
- Symlink `config.json` points to the file specified by `-DCONFIG_FILE`
- Default: `config_opti4001_ddcutil.json`
- Users can change the symlink after installation

### Systemd Service Template

**Template File:** [systemd/als-dimmer.service.in](../systemd/als-dimmer.service.in)

**Features:**
- `@CMAKE_INSTALL_PREFIX@` substitution for flexible installation paths
- Runs as root for I2C device access
- Security hardening (NoNewPrivileges, ProtectSystem, DeviceAllow)
- Resource limits (50MB memory, 10% CPU)
- Automatic restart on failure
- Journal logging (stdout/stderr)

**Generated Service File Example:**
```ini
[Unit]
Description=ALS-Dimmer: Ambient Light Sensor Based Display Brightness Control
After=network.target multi-user.target
Wants=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/als-dimmer --config /usr/local/etc/als-dimmer/config.json
ExecReload=/bin/kill -HUP $MAINPID
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

# Run as root for I2C device access
User=root

# Security hardening
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/als-dimmer /tmp
DeviceAllow=/dev/i2c-1 rw
DeviceAllow=/dev/i2c-0 rw
DevicePolicy=closed

# Resource limits
LimitNOFILE=1024
MemoryMax=50M
CPUQuota=10%

[Install]
WantedBy=multi-user.target
```

### Usage

**Enable and Start Service:**
```bash
# Reload systemd to recognize new service
sudo systemctl daemon-reload

# Enable service to start on boot
sudo systemctl enable als-dimmer

# Start service now
sudo systemctl start als-dimmer

# Check status
sudo systemctl status als-dimmer

# View logs
sudo journalctl -u als-dimmer -f
```

**Changing Default Config:**
```bash
# Change symlink to different config
cd /usr/local/etc/als-dimmer
sudo ln -sf config_fpga_opti4001_dimmer200.json config.json

# Restart service to pick up new config
sudo systemctl restart als-dimmer
```

**Benefits:**
- Flexible installation prefix for different deployment scenarios
- All config files installed as examples for easy switching
- Secure systemd service with resource limits
- Works with standard system installations and embedded rootfs builds
- Template-based approach ensures correct paths regardless of installation prefix

---

**Document Version:** 1.4
**Last Updated:** 2025-11-08
**Status:** Phase 2.5 COMPLETE AND TESTED - Production Ready with Zone-Based Mapping, Smooth Transitions, I2C Dimmer Support, FPGA Sensor, and Systemd Integration
