# ALS-Dimmer

Ambient Light Sensor Based Display Brightness Control daemon.

## Quick Start on Raspberry Pi 4

### Prerequisites

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get install -y git build-essential cmake pkg-config
sudo apt-get install -y libi2c-dev i2c-tools
sudo apt-get install -y libddcutil-dev  # For DDC/CI monitor support

# Enable I2C
sudo raspi-config
# Interface Options → I2C → Enable
# Reboot if needed
```

### Build

```bash
git clone https://github.com/hackboxguy/als-dimmer.git
cd ~/als-dimmer
cmake -H. -BOutput -DUSE_DDCUTIL=ON -DINSTALL_SYSTEMD_SERVICE=ON -DCONFIG_FILE=config_opti4001_ddcutil.json -DCMAKE_INSTALL_PREFIX=/home/pi/als-dimmer-install/
cmake --build Output -- install -j$(nproc)
sudo systemctl enable /home/pi/als-dimmer-install/lib/systemd/system/als-dimmer.service
sudo systemctl restart als-dimmer.service
 ~/als-dimmer-install/bin/als-dimmer-client --status
 
# Basic build (no DDC/CI support)
cmake -DCMAKE_BUILD_TYPE=Release ..

# With DDC/CI support for monitors
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Release ..

# With systemd service installation
cmake -DUSE_DDCUTIL=ON \
      -DINSTALL_SYSTEMD_SERVICE=ON \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_BUILD_TYPE=Release ..

# Compile
make -j$(nproc)

# Install (optional, for systemd service)
sudo make install
```

**CMake Configuration Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `USE_DDCUTIL` | OFF | Enable DDC/CI monitor support via libddcutil |
| `INSTALL_SYSTEMD_SERVICE` | OFF | Install systemd service file |
| `CONFIG_FILE` | config_opti4001_ddcutil.json | Default config file to use |
| `CMAKE_INSTALL_PREFIX` | /usr/local | Installation directory prefix |
| `CMAKE_BUILD_TYPE` | Release | Build type (Release, Debug, RelWithDebInfo) |

**Examples:**

```bash
# Development build with debug symbols
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# Production install with systemd and custom config
cmake -DUSE_DDCUTIL=ON \
      -DINSTALL_SYSTEMD_SERVICE=ON \
      -DCONFIG_FILE=config_fpga_opti4001_dimmer200.json \
      -DCMAKE_INSTALL_PREFIX=/usr \
      -DCMAKE_BUILD_TYPE=Release ..
make -j4
sudo make install
sudo systemctl enable als-dimmer
sudo systemctl start als-dimmer
```

### Verify Hardware

```bash
# Check I2C devices
i2cdetect -y 1

# Should see OPTI4001 at address 0x44 (or 0x45)
# Example output:
#      0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
# 40: -- -- -- -- 44 -- -- -- -- -- -- -- -- -- -- --

# Test DDC/CI monitor (if applicable)
ddcutil detect
ddcutil getvcp 10  # Get current brightness
```

### Run

```bash
# Run with OPTI4001 sensor and DDC/CI monitor
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground

# Run with CAN ALS sensor (requires SocketCAN setup)
sudo ip link set up can0 type can bitrate 500000
./als-dimmer --config ../configs/config_can_als_file.json --foreground

# Run in simulation mode (for testing without hardware)
./als-dimmer --config ../configs/config_simulation.json --foreground
```

### Control the Daemon

#### Using the Client Utility

The `als-dimmer-client` provides a convenient command-line interface for controlling the daemon:

```bash
# Get daemon status (mode, brightness, lux, zone)
./als-dimmer-client --status

# Get current brightness
./als-dimmer-client --brightness

# Set brightness to 75%
./als-dimmer-client --brightness=75

# Adjust brightness by +10%
./als-dimmer-client --adjust=10

# Get current mode
./als-dimmer-client --mode

# Switch to manual mode
./als-dimmer-client --mode=manual

# Switch to auto mode
./als-dimmer-client --mode=auto

# Connect via Unix socket (lower latency)
./als-dimmer-client --use-unix-socket --status

# Connect to remote daemon
./als-dimmer-client --ip=192.168.1.100 --port=9000 --status

# Get raw JSON response
./als-dimmer-client --status --json
```

##### Absolute brightness (nits)

When a brightness-to-nits calibration table is loaded (see `brightness_to_nits` in
the config and `tools/als-dimmer-sweep.py`), the daemon can also report and accept
absolute luminance in nits:

```bash
# Discover the LUT range so a UI can size its slider
./als-dimmer-client --max-brightness        # e.g. 1119.8
./als-dimmer-client --min-brightness        # e.g. 0.05

# Read the panel's current absolute luminance
./als-dimmer-client --absolute-brightness   # e.g. 749.2

# Set a target in nits (clamped to LUT range, returns the % the daemon used)
./als-dimmer-client --absolute-brightness=750

# Inspect the loaded LUT (label, output_type tag, row count)
./als-dimmer-client --calibration-info
```

When no LUT is loaded, the numeric queries print `(uncalibrated)` to stderr and
exit with code 6, so scripts can probe with `if als-dimmer-client --max-brightness`.
`set_absolute_brightness` errors with `CALIBRATION_NOT_LOADED` to avoid fabricating
a nits-to-% mapping.

#### Using the JSON Protocol Directly

The daemon supports both TCP and Unix domain sockets with a JSON-based protocol:

```bash
# Get full status (mode, brightness, lux, zone)
printf '{"version":"1.0","command":"get_status"}' | nc -w 1 localhost 9000

# Response (in AUTO mode):
# {"version":"1.0","status":"success","message":"Status retrieved successfully","data":{"mode":"auto","brightness":75,"lux":450.5,"zone":"indoor"}}

# Response (in MANUAL_TEMPORARY mode, after setting brightness in AUTO):
# {"version":"1.0","status":"success","message":"Status retrieved successfully","data":{"mode":"manual_temporary","brightness":80,"lux":450.5,"zone":"indoor"}}

# Response (in MANUAL mode, after explicit mode change):
# {"version":"1.0","status":"success","message":"Status retrieved successfully","data":{"mode":"manual","brightness":50,"lux":450.5,"zone":"indoor"}}

# Get configuration
printf '{"version":"1.0","command":"get_config"}' | nc -w 1 localhost 9000

# Manual brightness adjustment (auto-resumes after 60 seconds)
printf '{"version":"1.0","command":"set_brightness","params":{"brightness":75}}' | nc -w 1 localhost 9000

# Adjust brightness by delta
printf '{"version":"1.0","command":"adjust_brightness","params":{"delta":10}}' | nc -w 1 localhost 9000

# Switch to sticky manual mode
printf '{"version":"1.0","command":"set_mode","params":{"mode":"manual"}}' | nc -w 1 localhost 9000

# Resume automatic control
printf '{"version":"1.0","command":"set_mode","params":{"mode":"auto"}}' | nc -w 1 localhost 9000

# Test via Unix socket (same commands, lower latency)
printf '{"version":"1.0","command":"get_status"}' | nc -w 1 -U /tmp/als-dimmer.sock
```

**Available Commands:**
- `get_status` - Get system status (mode, brightness, lux, zone, sensor_status, calibrated, nits)
- `get_config` - Get configuration (mode, manual_brightness, last_auto_brightness, output_type, calibration metadata)
- `set_mode` - Set operating mode (`"auto"` or `"manual"`). Rejected with `SENSOR_UNAVAILABLE` when AUTO is requested but no sensor is reachable.
- `set_brightness` - Set brightness (0-100, triggers MANUAL_TEMPORARY in AUTO mode)
- `adjust_brightness` - Adjust brightness by delta value (-100 to +100)
- `get_absolute_brightness` - Get current brightness in nits. Returns `{"nits": null, "calibrated": false}` when no LUT is loaded.
- `set_absolute_brightness` - Set brightness via a target in nits (`{"nits": 750}`). Inverse-interpolates through the loaded LUT to a brightness %. Out-of-range targets are clamped with a `clamped: true` flag. Errors `CALIBRATION_NOT_LOADED` when no LUT is loaded.
- `get_calibration_info` - Get LUT diagnostics: `min_nits`, `max_nits`, `label`, `output_type`, `row_count`. Returns `{"calibrated": false}` when uncalibrated. Also reports thermal-compensation state when enabled (`thermal_enabled`, `backlight_temp_c`, `thermal_factor`, `thermal_reference_temp_c`, `thermal_factor_min`/`_max`, `thermal_label`).

## Operating Modes

The daemon supports three operating modes with seamless transitions:

- **AUTO**: Continuous sensor reading → automatic brightness adjustment based on ambient light
- **MANUAL**: Persistent manual brightness (survives daemon restarts)
- **MANUAL_TEMPORARY**: Temporary manual override with automatic return to AUTO mode after timeout (default: 60 seconds)

**Mode Behavior:**
- Setting brightness while in AUTO mode → Switches to MANUAL_TEMPORARY (allows quick adjustments without permanent mode change)
- Explicitly setting mode to "manual" → Switches to persistent MANUAL mode
- MANUAL_TEMPORARY timeout expires → Automatically returns to AUTO mode
- Status response exposes all three modes including the transitional `manual_temporary` state

### Sensor unavailable → MANUAL fallback

If the ALS sensor fails to initialize (no hardware wired, wrong I2C bus, etc.) or
goes unhealthy at runtime for longer than `control.sensor_failure_timeout_sec`
(default 30s), the daemon swaps in a `NullSensor` and forces MANUAL mode so the
display stays controllable from the slider. `get_status` reports
`"sensor_status": "unavailable"` in this state and `set_mode auto` is rejected
with `SENSOR_UNAVAILABLE` until the daemon is restarted with a working sensor.

## Configuration

See `configs/` directory for sample configurations:
- `config_opti4001_ddcutil.json` - OPTI4001 sensor + DDC/CI monitor
- `config_opti4001_boepwm.json` - OPTI4001 sensor + BOE display via MPS MPQ3367 + Pi PWM (with reference brightness-to-nits LUT)
- `config_fpga_opti4001_dimmer2048.json` - FPGA-bridged OPTI4001 + FPGA dimmer (16-bit native)
- `config_can_als_file.json` - CAN ALS sensor + file output (for testing)
- `config_simulation.json` - File-based simulation for testing

### Brightness-to-nits calibration

Optional top-level `brightness_to_nits` block points the daemon at a sweep table
that maps brightness % (0..100) to absolute luminance for the deployed
output+panel combo:

```json
"brightness_to_nits": {
  "enabled": true,
  "sweep_table": "calibrations/boe_pwm_2khz_reference.csv"
}
```

Relative `sweep_table` paths resolve against the config file's directory, so the
shipped configs work regardless of `CMAKE_INSTALL_PREFIX`. The `calibrations/`
directory is installed alongside `etc/als-dimmer/` by `cmake --install`.

A reference LUT for BOE displays is shipped pre-enabled in
`config_opti4001_boepwm.json`. It was swept on one specific unit so per-panel
variation of 5–15% is normal; for accurate readings on your panel, run
`tools/als-dimmer-sweep.py` (drives the daemon's `set_brightness` and a
colorimeter through `spotread`) and replace the file.

### Thermal compensation (optional)

The brightness-to-nits LUT captures the panel's response at the temperature
the sweep was taken at. Once the panel reaches thermal equilibrium (~30 min
after power-on), LED junction efficiency and phosphor yield drop and the
same brightness % produces 4–8% fewer nits. To compensate, the daemon can
optionally:

1. Poll a user-supplied shell command to read backlight temperature (e.g.
   via the F1KM I2C NTC: `disptool --device=ioc --command=bltemp ...`)
2. Look up a multiplicative correction factor in a small CSV (produced by
   `tools/thermal-factor.py` from a "panel running at 100% over a warm-up
   period" measurement log)
3. Apply that factor to all nits readings on both the GET and SET paths

Config block:

```json
"thermal_compensation": {
  "enabled": true,
  "factor_table": "calibrations/dimmer_12_3_nq1v1_thermal_factor.csv",
  "temp_command": "disptool --device=ioc --command=bltemp --autotestformat | sed -E 's/^.*Temperature[^:]*:\\s*([0-9.-]+).*/\\1/'",
  "poll_interval_sec": 30
}
```

The block is **optional** and **disabled by default** in shipped configs:
without it, the daemon behaves exactly as if the feature didn't exist.
When the temp command keeps failing, factor falls back to 1.0 so
brightness-to-nits readings stay sensible even if the temp source breaks.

## Troubleshooting

### I2C Permission Denied

```bash
# Add user to i2c group
sudo usermod -a -G i2c $USER
# Log out and back in
```

### OPTI4001 Not Detected

```bash
# Check I2C bus
i2cdetect -y 1

# Check connections
# - SDA → Pin 3 (GPIO 2)
# - SCL → Pin 5 (GPIO 3)
# - VCC → Pin 1 or 17 (3.3V)
# - GND → Pin 6, 9, 14, 20, 25, 30, 34, or 39
```

### DDC/CI Not Working

```bash
# Check monitor supports DDC/CI
ddcutil detect

# Try with sudo first
sudo ddcutil getvcp 10

# If works with sudo, add user to i2c group
sudo usermod -a -G i2c $USER
```

### Systemd Service NAMESPACE Error (226)

If the systemd service fails with `status=226/NAMESPACE`, the security directives may be too restrictive for your system:

```bash
# Verify the generated service file content
cat /path/to/install/lib/systemd/system/als-dimmer.service

# Check if ProtectSystem, DevicePolicy are commented out
grep -E "^(ProtectSystem|DevicePolicy|DeviceAllow)" /path/to/install/lib/systemd/system/als-dimmer.service

# If the directives are still active (not commented), rebuild with clean CMake cache:
cd ~/als-dimmer
rm -rf build
mkdir build && cd build
cmake -DUSE_DDCUTIL=ON -DINSTALL_SYSTEMD_SERVICE=ON -DCMAKE_INSTALL_PREFIX=/usr ..
make -j4
sudo make install
sudo systemctl daemon-reload
sudo systemctl restart als-dimmer

# Check journalctl for detailed error messages
sudo journalctl -u als-dimmer.service -n 50 --no-pager
```

**Note**: The service file has all security directives commented out by default for maximum compatibility with various systemd versions and kernel configurations. On Raspberry Pi OS and similar systems, even basic directives like `NoNewPrivileges=true` can cause NAMESPACE errors. Once the service is running successfully, you can gradually uncomment security directives in the service file to find which ones your system supports.

## Documentation

- [CLAUDE.md](docs/CLAUDE.md) - Complete design specification
- [PROGRESS.md](docs/PROGRESS.md) - Development progress and testing notes

