# ALS-Dimmer Development Progress

## Phase 1: Basic Functionality

**Goal:** Minimal working system with OPTI4001 sensor, DDC/CI output, file-based simulation, and TCP control interface.

### Incremental Steps

#### Step 1: Project skeleton + Config parsing
**Status:** ✅ COMPLETED
**Tasks:**
- [x] CMake structure with C++14, target-based configuration
- [x] Directory layout (include/, src/, configs/, third_party/)
- [x] Vendor nlohmann/json header
- [x] Config class for JSON parsing
- [x] Configuration validation (required vs optional fields)
- [x] Test: Load and validate config files

**Test Plan:**
```bash
# Should successfully parse valid config
./test_config configs/config_simulation.json

# Should report errors for invalid config
./test_config configs/config_invalid.json
```

---

#### Step 2: Abstract interfaces + File-based sensor/output (simulation)
**Status:** ✅ COMPLETED
**Tasks:**
- [x] `SensorInterface` abstract base class (init, readLux, isHealthy, getType)
- [x] `OutputInterface` abstract base class (init, setBrightness, getCurrentBrightness, getType)
- [x] `FileSensor` implementation (reads lux from text file)
- [x] `FileOutput` implementation (writes brightness to text file)
- [x] Simple main loop to test sensor → output flow

**Test Plan:**
```bash
# Terminal 1: Run daemon in simulation mode
./als-dimmer --config configs/config_simulation.json --foreground

# Terminal 2: Drive sensor input
echo "100" > /tmp/lux.txt
sleep 1
cat /tmp/brightness.txt  # Should show calculated brightness

echo "1000" > /tmp/lux.txt
sleep 1
cat /tmp/brightness.txt  # Should show different brightness
```

---

#### Step 3: OPT4001 sensor implementation
**Status:** ✅ COMPLETED
**Tasks:**
- [x] `OPTI4001Sensor` class implementation
- [x] I2C device initialization and communication
- [x] Device ID verification (0x121)
- [x] Sensor configuration (auto-range, continuous mode)
- [x] Lux reading with proper scaling (20-bit mantissa + 4-bit exponent)
- [x] Error handling for I2C failures
- [x] Correct register addresses: 0x00/0x01 for data, 0x0A for config, 0x11 for device ID
- [x] Proper lux calculation formula matching Linux kernel driver

**Test Results:**
```bash
# Tested on Raspberry Pi 4 with OPT4001 at I2C address 0x44
./als-dimmer --config configs/config_opti4001_ddcutil.json --foreground

# ✅ Device ID correctly read: 0x121
# ✅ Sensor readings: ~600 lux (indoor lighting)
# ✅ Dynamic response verified: 5-609 lux range observed
# ✅ Auto-ranging working: exponent=2 for indoor light levels
# ✅ Counter incrementing correctly: 0x0 → 0x1 → 0x2 → 0x3...
```

**Hardware:** Raspberry Pi 4 with OPT4001 at I2C address 0x44

---

#### Step 4: DDC/CI output implementation
**Status:** ✅ COMPLETED
**Tasks:**
- [x] CMake option USE_DDCUTIL with conditional compilation
- [x] `DDCUtilOutput` class implementation
- [x] Display detection and initialization
- [x] Brightness setting via VCP feature 0x10
- [x] Current brightness reading
- [x] Error handling for DDC/CI communication

**Implementation Details:**
- DDCUtilOutput class in src/outputs/ddcutil_output.cpp
- Conditional compilation with HAVE_DDCUTIL define
- Full libddcutil C API integration
- Display enumeration and selection by index
- VCP feature 0x10 (brightness) read/write
- Proper resource cleanup in destructor

**Test Plan:**
```bash
# Build with DDC/CI support
cmake -DUSE_DDCUTIL=ON ..
make

# Test DDC/CI output on monitor
./als-dimmer --config configs/config_opti4001_ddcutil.json --foreground

# Should see monitor brightness change based on lux
# Verify with: ddcutil getvcp 10
```

---

#### Step 5: Basic brightness controller (single zone, simple rate limiting)
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Simple linear brightness mapping (0-1000 lux → 5-100%)
- [x] Main control loop integration with operating modes
- [x] Rate-limited control via update_interval_ms
- [x] End-to-end AUTO mode functionality

**Implementation Details:**
- Linear mapping function in main.cpp (mapLuxToBrightness)
- Main control loop with mode-aware brightness control
- AUTO mode: sensor → mapping → output
- MANUAL/MANUAL_TEMPORARY: uses stored brightness value
- Update interval provides natural rate limiting

**Note:** Advanced features deferred to Phase 2:
- Multi-zone mapping with different curves
- Error-based step sizes (LARGE/MEDIUM/SMALL)
- Smooth transitions during zone changes

**Test Plan:**
```bash
# End-to-end test: OPT4001 → DDC/CI
./als-dimmer --config configs/config_opti4001_ddcutil.json --foreground

# Cover/uncover sensor → brightness should adjust
# Check logs for: lux → target brightness → actual brightness
```

---

#### Step 6: State management + Operating modes
**Status:** Not Started
**Tasks:**
- [ ] `StateManager` class implementation
- [ ] Persistent state file (JSON format)
- [ ] State load/save with error handling
- [ ] Operating mode enum (AUTO, MANUAL, MANUAL_TEMPORARY)
- [ ] Mode transition logic
- [ ] Auto-resume timer for MANUAL_TEMPORARY mode
- [ ] Startup behavior based on saved state

**Test Plan:**
```bash
# Test AUTO mode
./als-dimmer --config config.json --foreground
# Should start in AUTO mode, adjust based on sensor

# Test mode persistence
# (in another terminal)
echo "SET_MODE manual" | nc localhost 9000
echo "SET_BRIGHTNESS 75" | nc localhost 9000
# Kill daemon, restart
./als-dimmer --config config.json --foreground
# Should resume in MANUAL mode at 75% brightness

# Test MANUAL_TEMPORARY auto-resume
# Start in AUTO mode
echo "SET_BRIGHTNESS 80" | nc localhost 9000  # Triggers MANUAL_TEMPORARY
# Wait 60+ seconds
# Should auto-resume AUTO mode (check logs)
```

---

#### Step 7: TCP control interface
**Status:** Not Started
**Tasks:**
- [ ] `ControlInterface` class for TCP socket handling
- [ ] Socket listener on configurable address:port
- [ ] Command parser (text-based protocol)
- [ ] Implement commands: GET_STATUS, GET_MODE, GET_BRIGHTNESS, GET_LUX
- [ ] Implement commands: SET_MODE, SET_BRIGHTNESS, SET_MANUAL_BRIGHTNESS
- [ ] Implement commands: GET_STATE, SAVE_STATE, RELOAD_CONFIG, SHUTDOWN
- [ ] Response formatting (OK/ERROR)
- [ ] Multi-client support

**Test Plan:**
```bash
# Terminal 1: Run daemon
./als-dimmer --config config.json --foreground

# Terminal 2: Test commands
echo "GET_STATUS" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000
echo "GET_LUX" | nc localhost 9000
echo "GET_BRIGHTNESS" | nc localhost 9000

echo "SET_BRIGHTNESS 75" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000  # Should show manual_temporary

echo "SET_MODE manual" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000  # Should show manual

echo "SET_MODE auto" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000  # Should show auto

# Test interactive mode
nc localhost 9000
GET_STATUS
SET_BRIGHTNESS 50
GET_BRIGHTNESS
^C
```

---

## Phase 1 Completion Criteria

- [x] All Step 1-7 tasks completed
- [x] End-to-end test: OPT4001 sensor → brightness calculation → DDC/CI output
- [x] Simulation mode working (file-based I/O)
- [x] All three operating modes functional (AUTO, MANUAL, MANUAL_TEMPORARY)
- [x] State persistence across daemon restarts
- [x] TCP control interface responding to all basic commands
- [x] Code compiles with no warnings (-Wall -Wextra -Wpedantic -Werror)
- [x] Full end-to-end testing on Raspberry Pi 4 with DDC/CI monitor ✅

**Status:** ✅ Phase 1 COMPLETE AND TESTED! All functionality verified on hardware.

**Next:**
- Await decision on Phase 2 implementation
- Potential features: Zone-based mapping, smooth transitions, calibration

---

## Phase 2: Zone-Based Brightness Mapping

**Goal:** Multi-zone brightness mapping with different curve types for better control across full lux range (0-100,000 lux).

### Implementation Steps

#### Step 1: ZoneMapper Class Design and Implementation
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Create ZoneMapper header file with interface definition
- [x] Implement zone selection logic (selectZone method)
- [x] Implement linear curve calculation
- [x] Implement logarithmic curve calculation
- [x] Add getCurrentZoneName for debugging/logging
- [x] Handle edge cases (negative lux, out-of-range values)

**Files Created:**
- `include/als-dimmer/zone_mapper.hpp` - Class interface
- `src/zone_mapper.cpp` - Implementation with both curve types

---

#### Step 2: Main Loop Integration
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Add zone_mapper.hpp include to main.cpp
- [x] Initialize ZoneMapper from config.zones in main()
- [x] Update AUTO mode to use ZoneMapper instead of simple mapping
- [x] Add backward compatibility (fallback to simple mapping if no zones)
- [x] Update processCommand() to report zone information
- [x] Update logging to show zone name and curve type
- [x] Add zone_mapper.cpp to CMakeLists.txt

**Changes:**
- Renamed `mapLuxToBrightness()` → `mapLuxToBrightnessSimple()` for backward compatibility
- Added optional zone_mapper parameter to processCommand()
- Modified AUTO mode control logic to use zone mapper when available

---

#### Step 3: Testing and Validation
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Compile with zone_mapper.cpp in build system
- [x] Test night zone (0-10 lux, logarithmic curve)
- [x] Test indoor zone (10-500 lux, linear curve)
- [x] Test outdoor zone (500-100k lux, logarithmic curve)
- [x] Verify zone transitions are smooth
- [x] Verify backward compatibility (configs without zones)

**Test Results:**
```
Night zone (5 lux):    23% brightness (expected: 5-30% range)
Indoor zone (100 lux): 37% brightness (expected: 30-70% range)
Outdoor zone (8000 lux): 93% brightness (expected: 70-100% range)
```

All tests passed in simulation mode. Ready for hardware testing.

---

## Phase 2 Completion Criteria

- [x] ZoneMapper class implemented with linear and logarithmic curves
- [x] Zone selection logic working correctly
- [x] Integration with main control loop
- [x] Backward compatibility maintained
- [x] All three zones tested in simulation mode
- [x] Code compiles with no warnings
- [x] Hardware testing on Raspberry Pi 4 with OPT4001 sensor
- [x] Automotive use-case testing (tunnel/outdoor transitions)

**Status:** ✅ Phase 2 COMPLETE AND TESTED - Production Ready!

---

## Phase 2.5: Smooth Brightness Transitions

**Goal:** Eliminate jarring brightness changes with gradual ramping using error-based step sizing.

### Implementation Steps

#### Step 1: BrightnessController Class Design
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Design error-based step sizing algorithm
- [x] Create BrightnessController header file
- [x] Implement calculateNextBrightness() with smooth ramping
- [x] Support per-zone step sizes and thresholds
- [x] Handle simple mode fallback (default values)

**Files Created:**
- `include/als-dimmer/brightness_controller.hpp` - Class interface
- `src/brightness_controller.cpp` - Error-based step sizing implementation

**Algorithm:**
- Large error (> threshold_large): Use large_step for fast convergence
- Medium error (> threshold_small): Use medium_step for moderate ramping
- Small error: Use small_step for fine-tuning

---

#### Step 2: Main Loop Integration
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Add brightness_controller.hpp include to main.cpp
- [x] Initialize BrightnessController in main()
- [x] Update AUTO mode to use smooth transitions
- [x] Pass current zone to BrightnessController for zone-aware step sizing
- [x] Update logging to show Target/Current/Next brightness
- [x] Add brightness_controller.cpp to CMakeLists.txt

**Changes:**
- AUTO mode now calculates next_brightness incrementally instead of jumping to target
- Logging enhanced to show transition progress
- Zone-aware step sizing uses config values per zone

---

#### Step 3: Testing and Validation
**Status:** ✅ COMPLETED
**Tasks:**
- [x] Compile with brightness_controller.cpp
- [x] Test smooth transitions in simulation mode
- [x] Test on hardware (Raspberry Pi 4 + OPT4001 + DDC/CI)
- [x] Verify all zone transitions (night/indoor/outdoor)
- [x] Confirm no jarring brightness changes
- [x] Validate automotive use-case (tunnel scenarios)

**Test Results:** See Phase 2.5 Hardware Testing section above

---

## Phase 2.5 Completion Criteria

- [x] BrightnessController class implemented
- [x] Error-based step sizing working correctly
- [x] Integration with main control loop
- [x] Per-zone step sizes from configuration
- [x] Tested in simulation mode
- [x] Code compiles with no warnings
- [x] Hardware testing on Raspberry Pi 4
- [x] Automotive use-case validation (smooth tunnel transitions)

**Status:** ✅ Phase 2.5 COMPLETE AND TESTED - Production Ready!

---

## Notes & Decisions

### 2025-11-08: Phase 2 Implementation Decisions
- **Use-cases:** Automotive (primary) + desktop office monitoring (secondary)
- **Approach:** "Start simple first" - 3-zone configuration with 2 curve types
- **Curve selection:**
  - **Logarithmic** for night/outdoor: Fast response at low end, slow at high end (matches human perception)
  - **Linear** for indoor: Proportional response for normal office/indoor conditions
- **Zone boundaries:**
  - Night: 0-10 lux (very dark, indoor at night, tunnels)
  - Indoor: 10-500 lux (office, home, overcast outdoor)
  - Outdoor: 500-100k lux (bright daylight, direct sunlight)
- **Backward compatibility:** Configs without zones fall back to simple linear mapping
- **Integration approach:** Optional zone_mapper pointer, graceful degradation

### 2025-11-05: Initial Planning
- Development approach: Small incremental steps with testing after each
- Target hardware: Raspberry Pi 4 with OPTI4001 sensor
- OPTI4001 I2C address: TBD (0x44 or 0x45)
- Initial focus: DDC/CI output for Phase 1
- Will add custom I2C output in Phase 2 or 3

---

## Current Status

**Active Phase:** ✅ Phase 3 COMPLETE - Unix Domain Socket + JSON Protocol for Android IVI Integration
**Last Updated:** 2025-11-09
**Next Action:** Ready for Android IVI integration or Phase 4 planning

---

## Latest Addition: CAN ALS Sensor Support (2025-11-09) ✅

**Status:** Successfully implemented Linux SocketCAN-based ambient light sensor.

**Implementation:**
- ✅ [can_als_sensor.hpp](../include/als-dimmer/sensors/can_als_sensor.hpp) - CAN sensor class definition
- ✅ [can_als_sensor.cpp](../src/sensors/can_als_sensor.cpp) - SocketCAN implementation
- ✅ [main.cpp](../src/main.cpp) - Factory function integration (line 44-51)
- ✅ [config.hpp](../include/als-dimmer/config.hpp) - Added timeout_ms field
- ✅ [config.cpp](../src/config.cpp) - CAN config parsing and validation
- ✅ [CMakeLists.txt](../CMakeLists.txt) - Added to build sources (line 25)
- ✅ [config_can_als_file.json](../configs/config_can_als_file.json) - Example configuration

**Protocol:**
- 8-byte CAN message (ID 0x0A2) from ESP32 VEML7700 sensor
- 3-byte little-endian lux value (0-16,777,215 range)
- Status byte validation (0x00=OK, 0x01=Error)
- 16-bit checksum validation (sum of bytes 0-5)
- Sequence counter and config index tracking

**Features:**
- Linux SocketCAN integration (compatible with CANable USB-to-CAN dongles)
- Non-blocking socket with CAN ID filtering
- Checksum validation for message integrity
- Stale data detection with configurable timeout (default: 5000ms)
- Thread-safe cached lux value using std::atomic<float>
- Comprehensive error handling (interface errors, timeouts, invalid data)

**Configuration Example:**
```json
{
  "sensor": {
    "type": "can_als",
    "can_interface": "can0",
    "can_id": "0x0A2",
    "timeout_ms": 5000
  }
}
```

**Hardware Requirements:**
- CAN interface (e.g., CANable USB-to-CAN adapter, built-in automotive CAN)
- Linux kernel with SocketCAN support
- ESP32 or other CAN sender transmitting VEML7700 lux data on ID 0x0A2

**Benefits:**
- Automotive-grade CAN bus integration
- Remote ALS placement (sensor can be away from display)
- Works with standard USB-to-CAN adapters (no custom hardware)
- Message integrity via checksum validation
- Configurable timeout for failover scenarios

**Build Status:**
- ✅ Compiles successfully with no warnings
- ✅ All existing functionality preserved
- ⏸️ Pending hardware testing with live CAN bus

---

## Phase 3: Android IVI Integration (Unix Socket + JSON Protocol)

**Status:** ✅ COMPLETED AND TESTED

**Goal:** Enable seamless integration with Android-based IVI (In-Vehicle Infotainment) systems with a clean JSON-only protocol.

### Implemented Features

**1. Dual Socket Support** ✅
- TCP socket (127.0.0.1:9000) for development and debugging
- Unix domain socket (/tmp/als-dimmer.sock) for low-latency IPC
- Both sockets configurable independently via JSON config
- Concurrent multi-client support on both socket types

**2. JSON-Only Protocol** ✅
- Legacy text-based protocol removed (breaking change - acceptable as not deployed)
- Structured JSON command/response format
- Protocol version field (v1.0) for future compatibility
- Comprehensive error handling with error codes

**3. Configuration Schema** ✅
- Socket configuration in control section
- Separate TCP and Unix socket settings
- Configurable socket path, permissions, owner/group
- All example configs updated with dual socket support

**4. Unix Socket Management** ✅
- Socket created by als-dimmer daemon
- File permissions: configurable (default 0660)
- Stale socket cleanup on startup
- Proper socket cleanup on shutdown

### Implementation Summary

**Step 1: Config Schema Updates** ✅
- Added `tcp_socket` and `unix_socket` configuration sections
- Socket enable/disable flags
- Unix socket path, permissions, owner, group fields
- Updated all 7 config files

**Step 2: JSON Protocol Implementation** ✅
- Protocol helpers in [json_protocol.hpp](../include/als-dimmer/json_protocol.hpp) and [json_protocol.cpp](../src/json_protocol.cpp)
- Command parsing with validation
- Response generation (success/error)
- Protocol version checking

**Step 3: ControlInterface Refactoring** ✅
- Dual socket listener threads (TCP + Unix)
- Shared JSON command processing logic
- Request/response model implemented
- Thread-safe client handling

**Step 4: Unix Socket Management** ✅
- Socket creation with proper permissions
- chown/chmod support (requires root)
- Stale socket detection and cleanup
- File descriptor management

**Step 5: Testing & Validation** ✅
- All JSON commands tested over TCP socket
- All JSON commands tested over Unix socket
- Error handling validated (parse errors, missing params, invalid values)
- Multi-client scenarios verified
- All config files updated and validated

### JSON Protocol Specification

**Commands (Client → Server):**
```json
{"version": "1.0", "command": "get_status"}
{"version": "1.0", "command": "get_config"}
{"version": "1.0", "command": "set_mode", "params": {"mode": "auto"}}
{"version": "1.0", "command": "set_brightness", "params": {"brightness": 75}}
{"version": "1.0", "command": "adjust_brightness", "params": {"delta": 10}}
```

**Responses (Server → Client):**
```json
// Success with data
{
  "version": "1.0",
  "status": "success",
  "message": "Status retrieved successfully",
  "data": {
    "mode": "auto",
    "brightness": 75,
    "lux": 450.5,
    "zone": "indoor"
  }
}

// Error
{
  "version": "1.0",
  "status": "error",
  "message": "Missing 'brightness' parameter",
  "data": {"error_code": "INVALID_PARAMS"}
}
```

**Supported Commands:**
- `get_status` - Get current system status (mode, brightness, lux, zone)
- `get_config` - Get configuration (mode, manual_brightness, last_auto_brightness)
- `set_mode` - Set operating mode (auto/manual)
- `set_brightness` - Set brightness (0-100, triggers MANUAL_TEMPORARY mode)
- `adjust_brightness` - Adjust brightness by delta value

**Error Codes:**
- `PARSE_ERROR` - Invalid JSON syntax
- `INVALID_PARAMS` - Missing or invalid parameters
- `UNKNOWN_COMMAND` - Unrecognized command type
- `INVALID_FORMAT` - Non-JSON command (legacy protocol rejected)
- `INTERNAL_ERROR` - Server-side error

### Configuration Example

```json
{
  "control": {
    "tcp_socket": {
      "enabled": true,
      "listen_address": "127.0.0.1",
      "listen_port": 9000
    },
    "unix_socket": {
      "enabled": true,
      "path": "/tmp/als-dimmer.sock",
      "permissions": "0660",
      "owner": "root",
      "group": "root"
    },
    "update_interval_ms": 500,
    "log_level": "info"
  }
}
```

### Test Results

**JSON Protocol Tests:**
```bash
# GET_STATUS over TCP: ✅
{"data":{"brightness":30,"lux":50.0,"mode":"auto","zone":"night"},"message":"Status retrieved successfully","status":"success","version":"1.0"}

# SET_BRIGHTNESS over Unix socket: ✅
{"data":{"brightness":75},"message":"Brightness set successfully","status":"success","version":"1.0"}

# ADJUST_BRIGHTNESS over TCP: ✅
{"data":{"brightness":85,"delta":10},"message":"Brightness adjusted successfully","status":"success","version":"1.0"}

# GET_CONFIG over Unix socket: ✅
{"data":{"last_auto_brightness":30,"manual_brightness":85,"mode":"manual"},"message":"Configuration retrieved successfully","status":"success","version":"1.0"}
```

**Error Handling Tests:**
```bash
# Invalid JSON: ✅
{"data":{"error_code":"PARSE_ERROR"},"message":"JSON parse error: ...","status":"error","version":"1.0"}

# Missing parameter: ✅
{"data":{"error_code":"INVALID_PARAMS"},"message":"Missing 'brightness' parameter","status":"error","version":"1.0"}

# Invalid value: ✅
{"data":{"error_code":"INVALID_PARAMS"},"message":"Brightness must be 0-100","status":"error","version":"1.0"}

# Legacy text command: ✅
{"data":{"error_code":"INVALID_FORMAT"},"message":"Invalid command format. Only JSON protocol is supported...","status":"error","version":"1.0"}
```

### Files Modified/Created

**New Files:**
- [include/als-dimmer/json_protocol.hpp](../include/als-dimmer/json_protocol.hpp) - Protocol definitions and helpers
- [src/json_protocol.cpp](../src/json_protocol.cpp) - Command parsing and response generation

**Modified Files:**
- [include/als-dimmer/config.hpp](../include/als-dimmer/config.hpp) - Socket config structures
- [src/config.cpp](../src/config.cpp) - Socket config parsing
- [include/als-dimmer/control_interface.hpp](../include/als-dimmer/control_interface.hpp) - Dual socket support
- [src/control_interface.cpp](../src/control_interface.cpp) - Unix socket + JSON protocol
- [src/main.cpp](../src/main.cpp) - JSON-only command processing (~130 lines of legacy code removed)
- All 7 config files in [configs/](../configs/) - Added socket configuration

### Android Integration (Future Phase)

**Phase 3.1: Android System Service (Java/Kotlin)**
- Create privileged Android system app
- Connect to Unix domain socket (`/dev/socket/als-dimmer`)
- Parse JSON responses
- Expose Binder interface to Android framework
- Integrate with Settings, SystemUI, DisplayManager

**Phase 3.2: Production Hardening**
- SELinux policy for socket access
- init.rc socket creation (move from daemon to init)
- Persistent connection support
- Async event notifications (brightness changes, mode changes)

**Deployment Notes:**
- For Android: Change socket path to `/dev/socket/als-dimmer`
- Ensure proper SELinux context for socket file
- Daemon should run as root or with appropriate capabilities
- System service needs `LOCAL_SOCKET` permissions

---

## Previous Phases

### 2025-11-08: Systemd Service Installation ✅

**Status:** Successfully implemented flexible systemd service installation with CMake options.

**Implementation:**
- ✅ [systemd/als-dimmer.service.in](../systemd/als-dimmer.service.in) - Service template file
- ✅ [CMakeLists.txt](../CMakeLists.txt) - Updated with installation logic
- ✅ CMake option: `INSTALL_SYSTEMD_SERVICE` (default: OFF)
- ✅ CMake option: `CONFIG_FILE` (default: config_opti4001_ddcutil.json)
- ✅ Template-based service file with `@CMAKE_INSTALL_PREFIX@` substitution
- ✅ All config files installed to `${CMAKE_INSTALL_PREFIX}/etc/als-dimmer/`
- ✅ Symlink `config.json` → selected default config

**Features:**
- Optional systemd service installation via `-DINSTALL_SYSTEMD_SERVICE=ON`
- Flexible installation prefix support (works with /usr, /usr/local, /opt, etc.)
- Security hardening (NoNewPrivileges, ProtectSystem, DeviceAllow for I2C)
- Resource limits (50MB memory, 10% CPU quota)
- Automatic restart on failure
- Journal logging integration

**CMake Options:**
```bash
-DINSTALL_SYSTEMD_SERVICE=ON         # Install systemd service (default: OFF)
-DCONFIG_FILE=config_name.json       # Select default config file
-DCMAKE_INSTALL_PREFIX=/usr/local    # Installation prefix
```

**Installation Layout:**
```
${CMAKE_INSTALL_PREFIX}/
├── bin/als-dimmer
├── etc/als-dimmer/
│   ├── config.json -> config_opti4001_ddcutil.json  (symlink)
│   ├── config_fpga_opti4001_ddcutil.json
│   ├── config_fpga_opti4001_dimmer200.json
│   └── ... (all configs)
└── lib/systemd/system/als-dimmer.service  (if enabled)
```

**Benefits:**
- Works with standard installations and embedded rootfs builds
- All configs installed as examples for easy switching
- Service file has correct paths regardless of installation prefix
- Secure by default with systemd hardening features

### 2025-11-08: FPGA-Based OPT4001 Sensor ✅

**Status:** Successfully implemented support for FPGA-cached OPT4001 ambient light sensor.

**Architecture:**
- FPGA acts as I2C slave to Raspberry Pi (configurable address, default 0x1D)
- FPGA acts as I2C master to OPT4001 sensor
- FPGA maintains cached lux reading for fast Pi access

**Implementation:**
- ✅ [fpga_opti4001_sensor.cpp](../src/sensors/fpga_opti4001_sensor.cpp) - Complete sensor implementation
- ✅ [main.cpp](../src/main.cpp) - Factory function integration
- ✅ [config.cpp](../src/config.cpp) - Configuration validation for fpga_opti4001 type
- ✅ [CMakeLists.txt](../CMakeLists.txt) - Added to build sources

**I2C Protocol:**
- Write command: `0x00 0x00 0x00 0x0C` (4 bytes, fixed)
- Read response: 4 bytes (byte 0 reserved, bytes 1-3 = 24-bit lux value, big-endian)
- Conversion: `lux = raw_value * 0.64`
- Error detection: `0xFFFFFFFF` indicates FPGA/sensor failure

**Configuration Files Created:**
- ✅ [config_fpga_opti4001_ddcutil.json](../configs/config_fpga_opti4001_ddcutil.json)
- ✅ [config_fpga_opti4001_dimmer200.json](../configs/config_fpga_opti4001_dimmer200.json)

**Features:**
- Direct I2C communication using Linux I2C device interface
- Big-endian 24-bit value extraction
- Error handling for I2C failures and FPGA error conditions
- Debug output for first 10 readings
- Sanity check for out-of-range lux values (> 100k)

**Benefits:**
- Simplified Pi software (single I2C transaction per reading)
- FPGA handles OPT4001 timing and initialization
- Fast readings (< 5ms per transaction)
- Suitable for automotive FPGA-based ECU deployments

### 2025-11-08: Logging System Implementation ✅

**Status:** Successfully implemented custom logger with runtime filtering and updated core components.

**Features:**
- ✅ 5 log levels: TRACE, DEBUG, INFO, WARN, ERROR
- ✅ Runtime log level filtering (no recompilation needed)
- ✅ Thread-safe logging with mutex protection
- ✅ Consistent timestamp format: `[YYYY-MM-DD HH:MM:SS] [LEVEL] [Component] Message`
- ✅ Configuration file support: `control.log_level`
- ✅ Command-line override: `--log-level <level>`

**Components Updated with New Logger:**
1. ✅ [main.cpp](../src/main.cpp) - Control loop
2. ✅ [state_manager.cpp](../src/state_manager.cpp) - Persistent state
3. ✅ [file_sensor.cpp](../src/sensors/file_sensor.cpp) - File-based sensor
4. ✅ [file_output.cpp](../src/outputs/file_output.cpp) - File-based output
5. ✅ [zone_mapper.cpp](../src/zone_mapper.cpp) - Zone selection
6. ✅ [control_interface.cpp](../src/control_interface.cpp) - TCP control

**Components Still Using Old Format:**
- [opti4001_sensor.cpp](../src/sensors/opti4001_sensor.cpp) - Hardware I2C sensor
- [i2c_dimmer_output.cpp](../src/outputs/i2c_dimmer_output.cpp) - Custom dimmer output

**Test Results:**
```bash
# INFO level (production): Clean, minimal output
[2025-11-08 22:18:30] [INFO ] [main] ALS-Dimmer starting (log level: info)
[2025-11-08 22:18:30] [INFO ] [main] Configuration loaded
[2025-11-08 22:18:30] [INFO ] [main] Sensor initialized: file

# DEBUG level (troubleshooting): Detailed component logging
[2025-11-08 22:22:33] [DEBUG] [StateManager] State loaded: mode=auto, manual_brightness=50
[2025-11-08 22:22:33] [DEBUG] [FileSensor] Initializing with file: /tmp/als-test.lux
[2025-11-08 22:22:33] [DEBUG] [ZoneMapper] Lux=250 Zone=indoor Curve=linear Brightness=49%
```

**Benefits:**
- Production deployments can run at INFO level (quiet, essential logs only)
- Troubleshooting can use DEBUG level (detailed component activity)
- No log spam during normal operation
- Easy to track issues with timestamps and component names

### 2025-11-08: Custom I2C Dimmer Output Implementation ✅

**Status:** Successfully implemented support for dimmer200 and dimmer800 displays.

**Implementation:**
- ✅ [i2c_dimmer_output.hpp](../include/als-dimmer/outputs/i2c_dimmer_output.hpp) - Header with enum and class definition
- ✅ [i2c_dimmer_output.cpp](../src/outputs/i2c_dimmer_output.cpp) - Direct I2C register writes
- ✅ Automatic scaling: 0-100% → 0-200 or 0-800
- ✅ Factory functions: `createDimmer200Output()` and `createDimmer800Output()`
- ✅ Integration with main.cpp output factory
- ✅ Configuration validation for dimmer types

**Dimmer Types:**
- **dimmer200:** 0-200 native range (basic automotive displays)
- **dimmer800:** 0-800 native range (high-resolution automotive displays)
- Both use I2C register 0x50 for brightness control
- Typical I2C address: 0x1D

**Configuration Files Created:**
- ✅ [config_opti4001_dimmer200.json](../configs/config_opti4001_dimmer200.json)
- ✅ [config_opti4001_dimmer800.json](../configs/config_opti4001_dimmer800.json)

**Benefits:**
- Fast brightness control (< 10ms, no DDC/CI overhead)
- Direct register access for automotive displays
- Automatic scaling maintains 0-100% internal representation
- Ready for automotive deployment with custom displays

### 2025-11-08: Phase 2.5 Smooth Transitions TESTED ON HARDWARE ✅

**Implementation:**
- ✅ BrightnessController class created ([brightness_controller.hpp](../include/als-dimmer/brightness_controller.hpp), [brightness_controller.cpp](../src/brightness_controller.cpp))
- ✅ Error-based step sizing algorithm (LARGE/MEDIUM/SMALL steps)
- ✅ Per-zone step sizes and thresholds from configuration
- ✅ Integrated into main AUTO mode control loop

**Algorithm:**
```
error = target_brightness - current_brightness

if |error| > threshold_large:  → large_step  (fast convergence)
elif |error| > threshold_small: → medium_step (moderate)
else:                           → small_step  (fine-tuning)
```

**Hardware Test Results (Raspberry Pi 4 + OPT4001 + DDC/CI):**

Indoor → Night (50% → 5% = 45% drop):
```
50% → 45% → 40% → 35% → 30% → 25% → 23% → 21% → 19% → ... → 5%
Time: ~4 seconds, smooth ramp-down
```

Night → Indoor (19% → 49% = 30% increase):
```
19% → 27% → 30% → 33% → 36% → 39% → 42% → 49%
Time: ~3.5 seconds, responsive recovery
```

Indoor → Outdoor (42% → 87% = 45% jump):
```
42% → 52% → 62% → 66% → 70% → 74% → 78% → ... → 87%
Time: ~4 seconds, smooth transition
```

Outdoor → Indoor (78% → 49% = 29% drop):
```
78% → 70% → 67% → 64% → 61% → 58% → 55% → 54% → 53% → 52% → 51% → 50% → 49%
Time: ~6 seconds, very smooth ramp-down
```

**Performance:**
- ✅ No jarring brightness changes (critical for automotive safety)
- ✅ Responsive to large changes (3-6 seconds for full transition)
- ✅ Smooth fine-tuning prevents oscillation
- ✅ Zone-aware step sizing (outdoor uses larger steps)
- ✅ Professional feel matching commercial products

**Status:** Phase 2.5 smooth transitions are **production-ready** and significantly improve user experience!

### 2025-11-08: Phase 2 Hardware Testing PASSED ✅

**Test Hardware:**
- Raspberry Pi 4
- OPT4001 sensor at I2C address 0x44 (I2C bus 1)
- DDC/CI capable monitor

**Zone Transition Test Results:**
```
Indoor → Night (tunnel entry):   Lux 250→0,    Brightness 49%→5%  ✅
Night → Indoor (tunnel exit):    Lux 0→248,    Brightness 5%→49%  ✅
Indoor → Outdoor (bright light):  Lux 250→1310, Brightness 49%→87% ✅
Outdoor → Indoor (normal light):  Lux 1310→250, Brightness 87%→49% ✅
```

**Zone Selection Accuracy:**
- Night zone (0 lux): 5% brightness ✅ (logarithmic curve, 5-30% range)
- Indoor zone (250 lux): 49% brightness ✅ (linear curve, 30-70% range)
- Outdoor zone (1310 lux): 87% brightness ✅ (logarithmic curve, 70-100% range)

**Performance Metrics:**
- Zone transitions: Instant and accurate ✅
- Sensor readings: Stable at ~245-256 lux (normal indoor lighting) ✅
- DDC/CI control: Smooth brightness adjustments ✅
- Console logging: Zone names and curves displayed correctly ✅
- 500ms update interval: Working smoothly ✅

**Automotive Use-Case Validation:**
- Tunnel simulation (covering sensor): Perfect night zone activation ✅
- Outdoor simulation (bright light source): Correct outdoor zone selection ✅
- Transitions smooth and responsive for driving scenarios ✅

**Status:** Phase 2 zone-based mapping is **production-ready** for both automotive and desktop use cases!

### 2025-11-08: Phase 2 Zone-Based Mapping IMPLEMENTATION ✅

**Implementation Details:**
- ✅ ZoneMapper class created ([zone_mapper.hpp](../include/als-dimmer/zone_mapper.hpp), [zone_mapper.cpp](../src/zone_mapper.cpp))
- ✅ Zone selection based on lux ranges (automatic, no user intervention needed)
- ✅ Linear curve implementation for proportional response (ideal for indoor)
- ✅ Logarithmic curve implementation for perceptual response (ideal for night/outdoor)
- ✅ Backward compatibility: falls back to simple linear mapping if no zones configured
- ✅ Main control loop integration with zone name logging
- ✅ GET_STATUS TCP command now reports current zone

**Test Results (Simulation Mode):**
- ✅ Night zone (5 lux): 23% brightness (logarithmic, 5-30% range)
- ✅ Indoor zone (100 lux): 37% brightness (linear, 30-70% range)
- ✅ Outdoor zone (8000 lux): 93% brightness (logarithmic, 70-100% range)
- ✅ Zone transitions smooth and automatic
- ✅ Logging shows zone name and curve type for debugging

**Configuration:**
- Three-zone configuration in [config_simulation.json](../configs/config_simulation.json) and [config_opti4001_ddcutil.json](../configs/config_opti4001_ddcutil.json)
- Night: 0-10 lux → 5-30%, logarithmic (fast response at low light, matches human perception)
- Indoor: 10-500 lux → 30-70%, linear (proportional response for normal conditions)
- Outdoor: 500-100k lux → 70-100%, logarithmic (prevents excessive brightness at high lux)

### 2025-11-05: Phase 1 End-to-End Testing PASSED ✅

**Test Hardware:**
- Raspberry Pi 4
- OPT4001 sensor at I2C address 0x44
- DDC/CI capable monitor

**Test Results:**
- ✅ OPT4001 sensor reading correctly: ~304 lux (indoor), Device ID 0x121
- ✅ DDC/CI display control working: 1 display detected, brightness control functional
- ✅ AUTO mode: Brightness tracking ambient light (lux 304 → brightness 33%)
- ✅ MANUAL mode: Sticky manual control persists across restarts
- ✅ MANUAL_TEMPORARY mode: Auto-resume after 60 seconds verified
- ✅ TCP control interface: All commands working (GET_STATUS, GET_MODE, SET_BRIGHTNESS, SET_MODE)
- ✅ State persistence: Mode and brightness restored after daemon restart
- ✅ Linear brightness mapping: Formula verified (5 + (lux/1000)*95)

**TCP Command Examples:**
```bash
# Use -q 0 flag to avoid Ctrl+C requirement
echo "GET_STATUS" | nc -q 0 localhost 9000
echo "SET_BRIGHTNESS 75" | nc -q 0 localhost 9000
echo "GET_MODE" | nc -q 0 localhost 9000
echo "SET_MODE auto" | nc -q 0 localhost 9000
```

**Performance:**
- Sensor readings stable and consistent
- Counter incrementing correctly (4-bit wraparound)
- DDC/CI communication reliable
- 500ms update interval working smoothly

### 2025-11-05: Steps 3-5 Completed (Phase 1 Implementation Complete!)

**Step 5 (Basic brightness controller):**
- ✅ Simple linear brightness mapping implemented
- ✅ Main control loop with mode-aware control
- ✅ AUTO mode: continuous sensor reading and brightness adjustment
- ✅ Rate limiting via configurable update interval
- ✅ End-to-end functionality working in simulation mode

**Step 4 (DDC/CI output):**
- ✅ DDCUtilOutput class fully implemented
- ✅ libddcutil C API integration
- ✅ Display detection and enumeration
- ✅ Brightness control via VCP feature 0x10
- ✅ Current brightness reading support
- ✅ CMake conditional compilation working

**Step 3 (OPT4001 sensor):**
- ✅ Complete sensor implementation with correct register addresses
- ✅ Device ID verification, auto-ranging, continuous mode
- ✅ Validated against Linux kernel driver
- ✅ Tested on Raspberry Pi 4: 5-609 lux range observed

### 2025-11-05: Steps 6-7 Completed (TCP Control + Operating Modes)
- ✅ StateManager class with JSON persistence
- ✅ ControlInterface with TCP socket listener (multi-client support)
- ✅ Text-based command protocol implemented
- ✅ All three operating modes working:
  - AUTO: Sensor → brightness calculation → output
  - MANUAL: Sticky manual brightness (persists across restarts)
  - MANUAL_TEMPORARY: Auto-resumes to AUTO after 60 seconds
- ✅ Mode transition logic tested:
  - SET_BRIGHTNESS in AUTO → MANUAL_TEMPORARY (auto-resume timer starts)
  - SET_MODE manual → MANUAL (sticky)
  - SET_MODE auto → AUTO (resume automatic control)
- ✅ TCP commands tested and working:
  - GET_STATUS (returns JSON with mode, lux, brightness, resume timer)
  - GET_MODE, GET_LUX, GET_BRIGHTNESS, GET_STATE
  - SET_BRIGHTNESS, SET_MODE, SET_MANUAL_BRIGHTNESS
  - SAVE_STATE, SHUTDOWN
- ✅ State persistence verified:
  - Saves to /tmp/als-dimmer-state.json
  - Mode and brightness restored on restart
  - Periodic saves (every 60s) + save on shutdown
- ✅ End-to-end simulation test successful:
  - Started in AUTO mode (lux=100 → brightness=14%)
  - Manual adjustment → MANUAL_TEMPORARY mode
  - Mode switching → MANUAL mode
  - Back to AUTO mode → responds to lux changes (lux=500 → brightness=52%)
  - Graceful shutdown via SHUTDOWN command

### 2025-11-05: Step 2 Completed
- ✅ Abstract interfaces defined (SensorInterface, OutputInterface)
- ✅ FileSensor implementation with error handling
- ✅ FileOutput implementation with range clamping
- ✅ Factory functions for creating sensors/outputs
- ✅ Main control loop with simple linear brightness mapping
- ✅ Command-line argument parsing (--config, --foreground, --help)
- ✅ End-to-end simulation test successful:
  - Reads lux from /tmp/als_lux.txt
  - Calculates brightness (0-1000 lux → 5-100%)
  - Writes brightness to /tmp/als_brightness.txt
  - Loop runs 10 iterations in test mode

### 2025-11-05: Step 1 Completed
- ✅ CMake build system created with USE_DDCUTIL option
- ✅ Project directory structure established
- ✅ nlohmann/json v3.11.3 vendored successfully
- ✅ Config class implemented with full JSON parsing
- ✅ Comprehensive validation for sensor, output, zones, and control settings
- ✅ Created 3 example configs: simulation, opti4001+ddcutil, and invalid (for testing)
- ✅ All tests pass:
  - Valid configs parse successfully
  - Invalid configs report proper error messages
  - Config validation catches missing required fields
