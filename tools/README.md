# ALS-Dimmer Tools

This directory contains supporting tools for the ALS-Dimmer daemon.

## `als-dimmer-sweep.py` — brightness-to-nits calibration sweep

Drives the running daemon through brightness 100..0% (or any range/step) and
records the displayed luminance at each step using an Argyll-CMS colorimeter
(`spotread`). The output CSV is in the format the daemon's `BrightnessToNitsLut`
consumes — point `brightness_to_nits.sweep_table` at it and restart the daemon to
enable accurate per-panel `--absolute-brightness` and `--max-brightness` queries.

The tool is **output-agnostic** — it speaks to the daemon's JSON protocol over
TCP or Unix socket, never the output device directly, so the same script works
for `boe_pwm`, `dimmer800`, `dimmer2048`, `fpga_sysfs_dimmer`, `ddcutil`, etc.

```bash
# Default warm sweep (100..0% in 1% steps, 3s settle)
./als-dimmer-sweep.py --output ./calibrations/my_panel_warm.csv --label warm

# Custom step + F1KM-MCU backlight NTC temperature recording
./als-dimmer-sweep.py --step 5 --label hot \
    --output ./calibrations/my_panel_hot.csv \
    --temp-cmd 'i2ctransfer -y 1 w2@0x66 0x10 0x02 r2@0x66 | python3 -c "import sys,struct; r=sys.stdin.read().split(); print(struct.unpack(\">h\", bytes(int(x,16) for x in r))[0]/10.0)"'

# See all flags + temp-source examples
./als-dimmer-sweep.py --help
```

The script saves the daemon's original mode and brightness, switches to MANUAL
for the sweep duration, and restores on exit (or on Ctrl-C, with a partial CSV
written so an interrupted run is never wasted). Pure stdlib Python — no pip
dependencies.

## `als-dimmer-client` — daemon control utility

Built and installed alongside the daemon. See `als-dimmer-client --help` for the
full surface; the absolute-brightness flags are documented in the project
[README](../README.md#absolute-brightness-nits).

---

## ALS-Dimmer CSV Visualization Tool

## Overview

The `visualize_csv.py` tool generates comprehensive visual analysis of ALS-Dimmer control loop behavior from CSV logs.

## Features

- **4-panel visualization:**
  1. Lux → Brightness response curve
  2. Error vs Step Size adaptation
  3. Step category distribution over time
  4. Zone transitions with change markers

- **Multiple output formats:** PNG, JPEG, PDF, SVG
- **Configurable resolution:** DPI control for publication-quality plots
- **Summary statistics:** Automatic calculation of key metrics
- **Interactive or batch mode:** Display plots or save directly to file

## Requirements

```bash
pip3 install pandas matplotlib
```

## Usage

### Basic Usage

```bash
# Default: Creates input.png and displays interactively
python3 visualize_csv.py --input=/tmp/data.csv

# Your requested syntax: Save to JPEG without displaying
python3 visualize_csv.py --input=/tmp/transition.csv --output=/tmp/control-loop-behaviour.jpg
```

### Advanced Options

```bash
# High-resolution PNG for publication (300 DPI)
python3 visualize_csv.py -i /tmp/data.csv -o /tmp/high-res.png --dpi=300 --no-show

# PDF report for documentation
python3 visualize_csv.py --input=/tmp/data.csv --output=/tmp/report.pdf --no-show

# Short form with interactive display
python3 visualize_csv.py -i /tmp/data.csv -o /tmp/plot.jpg

# Auto-detect format from extension
python3 visualize_csv.py -i /tmp/data.csv -o /tmp/plot.svg --no-show
```

### Command-Line Arguments

| Argument | Short | Required | Description |
|----------|-------|----------|-------------|
| `--input` | `-i` | Yes | Input CSV file path |
| `--output` | `-o` | No | Output image path (default: `<input>.png`) |
| `--dpi` | - | No | Image resolution (default: 150) |
| `--no-show` | - | No | Skip interactive display, save only |
| `--format` | - | No | Output format (auto-detected from extension) |

### Supported Formats

- **PNG** (default) - Lossless, best for detailed plots
- **JPEG/JPG** - Compressed, smaller file size
- **PDF** - Vector format, best for reports/documentation
- **SVG** - Vector format, editable in Inkscape/Illustrator

## Output Examples

### File Sizes (Typical)

- JPEG (150 DPI): ~160 KB
- PNG (300 DPI): ~240 KB
- PDF: ~24 KB (vector)

### Summary Statistics Output

```
============================================================
SUMMARY STATISTICS
============================================================
Total iterations: 1080
Duration: 540.1 seconds
Update interval: ~0.500 seconds
Lux range: 10.0 - 1000.0
Brightness range: 5% - 95%
Zones visited: night, indoor, outdoor
Zone changes: 12

Step category distribution:
large_up      156
medium_up     324
small_up      289
none          311

Average error: 2.45%
Max error: 85%
```

## Integration with ALS-Dimmer

### 1. Collect CSV Data

```bash
# Start daemon with CSV logging
sudo /home/pi/als-dimmer-install/bin/als-dimmer \
  --config /home/pi/als-dimmer-install/etc/als-dimmer/config.json \
  --csvlog /tmp/tuning_session.csv \
  --foreground
```

### 2. Simulate Transitions

```bash
# Example: Simulate car entering/exiting tunnel
for lux in 1000 500 100 50 10 5 10 50 100 500 1000; do
    echo "$lux" > /tmp/als_lux.txt
    sleep 5
done
```

### 3. Generate Visualization

```bash
# Stop daemon (Ctrl+C) then visualize
python3 tools/visualize_csv.py \
  --input=/tmp/tuning_session.csv \
  --output=/tmp/tunnel-transition.jpg \
  --dpi=200 \
  --no-show
```

## Analyzing Results

### Key Metrics to Monitor

1. **Convergence Speed:** How quickly brightness reaches target
2. **Oscillation:** Look for repeated overshooting in error plot
3. **Step Adaptation:** Verify step sizes decrease as error decreases
4. **Zone Transitions:** Check for smooth zone boundary crossings

### Tuning Guidelines

- **Too slow convergence:** Increase step sizes (`large_up`, `medium_up`)
- **Oscillation:** Decrease step sizes or increase thresholds
- **Abrupt changes:** Decrease `large_down` steps
- **Zone flickering:** Increase `hysteresis_percent`

## Troubleshooting

**Error: "Input file not found"**
- Check CSV file path is correct
- Ensure daemon ran long enough to flush buffer (>5 seconds)

**Error: "CSV file is empty"**
- CSV only has header row
- Run daemon longer or trigger zone transitions

**Error: "No module named 'pandas'"**
- Install dependencies: `pip3 install pandas matplotlib`

## Examples Gallery

### Night → Outdoor Transition

```bash
python3 visualize_csv.py -i /tmp/night_to_outdoor.csv -o /tmp/transition.png
```

Shows adaptive step sizing as brightness ramps from 5% → 95%

### Manual Override Testing

```bash
python3 visualize_csv.py -i /tmp/manual_test.csv -o /tmp/manual.pdf --no-show
```

Captures control loop behavior during MANUAL → AUTO transitions

### High-Frequency Lux Changes

```bash
python3 visualize_csv.py -i /tmp/rapid_changes.csv -o /tmp/stress_test.jpg --dpi=300
```

Tests control loop response to rapid lux variations
