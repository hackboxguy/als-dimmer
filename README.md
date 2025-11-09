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
- `get_status` - Get system status (mode, brightness, lux, zone)
- `get_config` - Get configuration (mode, manual_brightness, last_auto_brightness)
- `set_mode` - Set operating mode (`"auto"` or `"manual"`)
- `set_brightness` - Set brightness (0-100, triggers MANUAL_TEMPORARY in AUTO mode)
- `adjust_brightness` - Adjust brightness by delta value (-100 to +100)

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

## Configuration

See `configs/` directory for sample configurations:
- `config_opti4001_ddcutil.json` - OPTI4001 sensor + DDC/CI monitor
- `config_can_als_file.json` - CAN ALS sensor + file output (for testing)
- `config_simulation.json` - File-based simulation for testing

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

