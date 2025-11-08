# ALS-Dimmer

Ambient Light Sensor Based Display Brightness Control for automotive and embedded systems.

## Quick Start on Raspberry Pi 4

### Prerequisites

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config
sudo apt-get install -y libi2c-dev i2c-tools
sudo apt-get install -y libddcutil-dev  # For DDC/CI monitor support

# Enable I2C
sudo raspi-config
# Interface Options → I2C → Enable
# Reboot if needed
```

### Build

```bash
cd ~/als-dimmer
mkdir -p build && cd build

# Option 1: With DDC/CI support (for monitors)
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Release ..

# Option 2: Without DDC/CI (faster compilation)
cmake -DUSE_DDCUTIL=OFF -DCMAKE_BUILD_TYPE=Release ..

# Compile
make -j4
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

# Run in simulation mode (for testing without hardware)
./als-dimmer --config ../configs/config_simulation.json --foreground
```

### Test TCP Control

```bash
# In another terminal, test commands:

# Get current mode
echo "GET_MODE" | nc localhost 9000

# Get full status
printf "GET_STATUS\n" | nc -w 1 localhost 9000

# Manual brightness adjustment (auto-resumes after 60 seconds)
printf "SET_BRIGHTNESS 75\n" | nc -w 1 localhost 9000

# Switch to sticky manual mode
printf "SET_MODE manual\n" | nc -w 1 localhost 9000
printf "SET_BRIGHTNESS 50\n" | nc -w 1 localhost 9000

# Resume automatic control
printf "SET_MODE auto\n" | nc -w 1 localhost 9000

# Shutdown gracefully
printf "SHUTDOWN\n" | nc -w 1 localhost 9000
```

## Operating Modes

- **AUTO**: Continuous sensor reading → automatic brightness adjustment
- **MANUAL**: Sticky manual brightness (persists across restarts)
- **MANUAL_TEMPORARY**: Manual adjustment with 60-second auto-resume to AUTO mode

## Configuration

See `configs/` directory for sample configurations:
- `config_opti4001_ddcutil.json` - OPTI4001 sensor + DDC/CI monitor
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

## Documentation

- [CLAUDE.md](docs/CLAUDE.md) - Complete design specification
- [PROGRESS.md](docs/PROGRESS.md) - Development progress and testing notes

