# ALS-Dimmer End-to-End Testing Guide

## Phase 1 Testing - Complete System Validation

### Hardware Setup Required

**Raspberry Pi 4 Configuration:**
- OPT4001 ambient light sensor on I2C bus 1 (address 0x44)
- DDC/CI capable monitor connected via HDMI or DisplayPort
- Monitor must support DDC/CI brightness control (VCP feature 0x10)

### Pre-Testing Checklist

1. **Verify I2C sensor connection:**
```bash
# Check I2C devices
i2cdetect -y 1

# Should show device at address 0x44 (or 0x45)
```

2. **Verify DDC/CI monitor support:**
```bash
# Install ddcutil if not already present
sudo apt-get install ddcutil

# Detect displays
ddcutil detect

# Check current brightness
ddcutil getvcp 10

# Test brightness control
ddcutil setvcp 10 50
ddcutil setvcp 10 100
```

3. **Build with DDC/CI support:**
```bash
cd /home/testpc/git-repos/als-dimmer
mkdir -p build
cd build
cmake -DUSE_DDCUTIL=ON -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Verify build succeeded
./als-dimmer --help
```

---

## Test 1: Sensor Verification

**Goal:** Verify OPT4001 sensor is reading correctly

```bash
# Run daemon in foreground
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

**Expected output:**
```
[main] Configuration loaded successfully
[OPTI4001] Initializing on /dev/i2c-1 at address 0x44
[OPTI4001] Device ID: 0x121
[OPTI4001] Sensor configured (continuous mode, auto-range)
[main] Sensor initialized: opti4001
```

**Validation:**
- Device ID should be 0x121
- Lux readings should be displayed every 500ms (default update interval)
- Cover/uncover sensor → lux values should change dramatically

**Pass Criteria:**
- ✅ Sensor initializes without errors
- ✅ Device ID = 0x121
- ✅ Lux values change when sensor is covered/uncovered
- ✅ Typical indoor lighting: 100-1000 lux
- ✅ Covered sensor: <10 lux
- ✅ Bright light/flashlight: >5000 lux

---

## Test 2: DDC/CI Output Verification

**Goal:** Verify DDC/CI brightness control is working

**Test procedure:**

1. Start daemon:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. Watch for output initialization:
```
[DDCUtil] Initializing DDC/CI for display 0
[DDCUtil] Found 1 display(s)
[DDCUtil] Display opened successfully
[DDCUtil] Current brightness: XX%
[main] Output initialized: ddcutil
```

3. Observe brightness changes in AUTO mode
4. Monitor should adjust brightness based on ambient light

**Validation:**
```bash
# In another terminal, check brightness via ddcutil
watch -n 1 "ddcutil getvcp 10"
```

**Pass Criteria:**
- ✅ DDC/CI display detected and opened
- ✅ Current brightness read successfully
- ✅ Monitor brightness changes when sensor is covered/uncovered
- ✅ Brightness changes are smooth (not flickering)
- ✅ No DDC/CI communication errors

---

## Test 3: Auto Mode - Linear Brightness Mapping

**Goal:** Verify brightness mapping (0-1000 lux → 5-100%)

**Test procedure:**

1. Start daemon in AUTO mode:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. Create different lighting conditions:

| Condition | Expected Lux | Expected Brightness |
|-----------|--------------|---------------------|
| Covered sensor | ~0-10 | 5% (minimum) |
| Dim indoor | ~100 | ~15% |
| Normal indoor | ~300 | ~33% |
| Bright indoor | ~500 | ~52% |
| Window/outdoor | ~1000+ | 100% (maximum) |

3. Verify mapping formula: `brightness = 5 + (lux / 1000.0) * 95`

**Pass Criteria:**
- ✅ Minimum brightness: 5% (even in darkness)
- ✅ Maximum brightness: 100% (at 1000+ lux)
- ✅ Linear progression between extremes
- ✅ Brightness updates within 500ms of lux change

---

## Test 4: Operating Modes

### Test 4a: AUTO Mode

**Goal:** Verify continuous automatic brightness adjustment

```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

**Expected behavior:**
- Brightness adjusts continuously based on sensor
- Logs show: `[main] AUTO: Lux=XXX -> Brightness=YY%`
- Mode persists across restarts

**Pass Criteria:**
- ✅ Brightness tracks ambient light changes
- ✅ No manual intervention required
- ✅ Smooth transitions (not jumpy)

### Test 4b: MANUAL Mode (Sticky)

**Goal:** Verify manual brightness control persists

**Test procedure:**

1. Start daemon:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. In another terminal:
```bash
# Switch to manual mode
echo "SET_MODE manual" | nc localhost 9000

# Set specific brightness
echo "SET_BRIGHTNESS 75" | nc localhost 9000

# Verify mode
echo "GET_MODE" | nc localhost 9000
# Should return: OK manual
```

3. Restart daemon (Ctrl+C, then restart)

4. Check mode after restart:
```bash
echo "GET_MODE" | nc localhost 9000
# Should still be: OK manual

echo "GET_BRIGHTNESS" | nc localhost 9000
# Should be: OK 75
```

**Pass Criteria:**
- ✅ Manual mode persists across restarts
- ✅ Manual brightness value persists
- ✅ Sensor readings ignored in manual mode
- ✅ Brightness stays constant despite light changes

### Test 4c: MANUAL_TEMPORARY Mode (Auto-Resume)

**Goal:** Verify auto-resume after 60 seconds

**Test procedure:**

1. Start in AUTO mode:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. Make manual adjustment:
```bash
echo "SET_BRIGHTNESS 80" | nc localhost 9000
echo "GET_MODE" | nc localhost 9000
# Should return: OK manual_temporary
```

3. Check status:
```bash
echo "GET_STATUS" | nc localhost 9000
# JSON should include "manual_resume_in_sec": <countdown>
```

4. Wait 60+ seconds and check logs:
```
[main] Auto-resuming AUTO mode (timeout expired)
```

5. Verify mode switched back:
```bash
echo "GET_MODE" | nc localhost 9000
# Should return: OK auto
```

**Pass Criteria:**
- ✅ SET_BRIGHTNESS in AUTO → MANUAL_TEMPORARY
- ✅ Timer starts at 60 seconds
- ✅ GET_STATUS shows countdown
- ✅ Auto-resumes to AUTO after timeout
- ✅ Timer resets on subsequent manual adjustments
- ✅ Explicit SET_MODE cancels timer

---

## Test 5: TCP Control Interface

**Goal:** Verify all TCP commands work correctly

**Test procedure:**

```bash
# Terminal 1: Start daemon
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground

# Terminal 2: Test commands
nc localhost 9000
```

**Commands to test:**

| Command | Expected Response | Notes |
|---------|------------------|-------|
| `GET_STATUS` | JSON with mode, lux, brightness | Includes resume timer if MANUAL_TEMPORARY |
| `GET_LUX` | OK <value> | Current lux reading |
| `GET_BRIGHTNESS` | OK <value> | Current brightness (0-100) |
| `GET_MODE` | OK auto/manual/manual_temporary | Current mode |
| `GET_STATE` | JSON with state | Includes manual_brightness, last_auto_brightness |
| `SET_BRIGHTNESS 50` | OK Brightness set to 50 | Changes mode to MANUAL_TEMPORARY if in AUTO |
| `SET_MODE auto` | OK Mode set to auto | Switches to AUTO mode |
| `SET_MODE manual` | OK Mode set to manual | Switches to MANUAL mode (sticky) |
| `SET_MANUAL_BRIGHTNESS 60` | OK Manual brightness set to 60 | Sets MANUAL mode + brightness |
| `SAVE_STATE` | OK State saved | Forces state file write |
| `SHUTDOWN` | OK Shutting down | Graceful shutdown |

**Pass Criteria:**
- ✅ All commands return OK response (no errors)
- ✅ Multi-client support (multiple nc connections work)
- ✅ Commands execute immediately (<100ms)
- ✅ State changes persist across restarts

---

## Test 6: State Persistence

**Goal:** Verify state file saves and loads correctly

**Test procedure:**

1. Start daemon:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. Set manual mode and brightness:
```bash
echo "SET_MODE manual" | nc localhost 9000
echo "SET_BRIGHTNESS 75" | nc localhost 9000
```

3. Force state save:
```bash
echo "SAVE_STATE" | nc localhost 9000
```

4. Check state file:
```bash
cat /tmp/als-dimmer-state.json
```

**Expected state file:**
```json
{
  "version": 1,
  "mode": "manual",
  "manual_brightness": 75,
  "last_auto_brightness": <previous>,
  "last_updated": "2025-11-05T..."
}
```

5. Restart daemon and verify:
```bash
echo "GET_MODE" | nc localhost 9000      # Should be: manual
echo "GET_BRIGHTNESS" | nc localhost 9000  # Should be: 75
```

**Pass Criteria:**
- ✅ State file created at configured path
- ✅ JSON is valid and readable
- ✅ Mode persists across restarts
- ✅ Manual brightness persists
- ✅ MANUAL_TEMPORARY converts to AUTO on restart
- ✅ Periodic saves every 60 seconds
- ✅ Saves on graceful shutdown

---

## Test 7: Error Handling

### Test 7a: Sensor Disconnect (Simulated)

**Test procedure:**

1. Start daemon normally
2. Physically disconnect sensor (or use wrong I2C address in config)
3. Observe error messages

**Expected behavior:**
- Initialization fails with clear error message
- Daemon exits gracefully (no crash)

### Test 7b: DDC/CI Monitor Unavailable

**Test procedure:**

1. Disconnect monitor or use system without DDC/CI support
2. Start daemon

**Expected behavior:**
- Clear error: "No displays found" or "DDC/CI not supported"
- Daemon exits gracefully

### Test 7c: Invalid Config

**Test procedure:**

```bash
./als-dimmer --config ../configs/config_invalid.json --foreground
```

**Expected behavior:**
- Config validation errors printed
- Daemon exits before sensor/output initialization

---

## Test 8: Long-Term Stability

**Goal:** Verify daemon runs reliably for extended period

**Test procedure:**

1. Start daemon:
```bash
./als-dimmer --config ../configs/config_opti4001_ddcutil.json --foreground
```

2. Let run for 1+ hour with varying light conditions

**Monitoring:**
```bash
# Monitor memory usage
watch -n 10 "ps aux | grep als-dimmer"

# Monitor state file updates
watch -n 5 "ls -lh /tmp/als-dimmer-state.json"
```

**Pass Criteria:**
- ✅ No crashes or hangs
- ✅ Memory usage stable (<50MB)
- ✅ CPU usage minimal (<1%)
- ✅ State file updates every 60s
- ✅ Sensor continues reading correctly
- ✅ DDC/CI commands don't fail over time
- ✅ No error message spam in logs

---

## Test 9: Performance Metrics

**Goal:** Measure system performance

**Metrics to collect:**

| Metric | Target | Measurement |
|--------|--------|-------------|
| Sensor read time | <10ms | Time between lux readings |
| DDC/CI write time | <200ms | Time to set brightness |
| Update latency | <500ms | Lux change → brightness change |
| Memory usage | <50MB | Resident set size |
| CPU usage | <1% | Average over 1 minute |
| State save time | <10ms | Time to write JSON |

**Test procedure:**

Add timing logs or use external monitoring tools:
```bash
# CPU/Memory monitoring
pidstat -r -u 1 -p $(pgrep als-dimmer)

# I/O monitoring
iostat -x 1
```

---

## Test 10: Configuration Variants

**Goal:** Test different configuration options

### Test 10a: Different Update Intervals

Edit config: `control.update_interval_ms`
- Test: 100ms, 500ms, 1000ms, 2000ms
- Verify brightness update frequency matches config

### Test 10b: Different Auto-Resume Timeouts

Edit config: `control.auto_resume_timeout_sec`
- Test: 10s, 30s, 60s, 120s
- Verify MANUAL_TEMPORARY auto-resumes at correct time

### Test 10c: Different Listen Addresses

Edit config: `control.listen_address`
- Test: "127.0.0.1" (localhost only)
- Test: "0.0.0.0" (all interfaces)
- Verify TCP accessibility from local/remote

---

## Success Criteria Summary

**Phase 1 is considered COMPLETE and ready for production when:**

- ✅ All 10 test scenarios pass
- ✅ No crashes or errors during 1+ hour run
- ✅ Memory/CPU usage within targets
- ✅ All operating modes work correctly
- ✅ State persistence verified
- ✅ TCP control fully functional
- ✅ Sensor and output working reliably
- ✅ Brightness mapping is smooth and responsive

---

## Known Limitations (Phase 1)

These are **expected** limitations, to be addressed in Phase 2:

1. **Single zone only** - Uses simple linear mapping (no curves)
2. **No smooth transitions** - Brightness changes immediately
3. **No error-based step sizes** - No acceleration for large changes
4. **No calibration** - Mapping is hardcoded
5. **No sensor failover** - If sensor fails, daemon stops

These limitations are **acceptable for Phase 1** and will be addressed in Phase 2 development.

---

## Next Steps After Testing

1. **If all tests pass:**
   - Mark Phase 1 as COMPLETE
   - Document any quirks or observations
   - Plan Phase 2 features (zones, calibration)

2. **If tests fail:**
   - Document failures in PROGRESS.md
   - Fix bugs and re-test
   - Update test plan based on findings

---

**Test Report Template:**

```markdown
## Test Execution Report

**Date:** YYYY-MM-DD
**Tester:** [Name]
**Hardware:** Raspberry Pi 4 + OPT4001 + [Monitor Model]
**Software Version:** [git commit hash]

### Test Results

| Test | Status | Notes |
|------|--------|-------|
| Test 1: Sensor | PASS/FAIL | |
| Test 2: DDC/CI | PASS/FAIL | |
| Test 3: Mapping | PASS/FAIL | |
| Test 4: Modes | PASS/FAIL | |
| Test 5: TCP | PASS/FAIL | |
| Test 6: State | PASS/FAIL | |
| Test 7: Errors | PASS/FAIL | |
| Test 8: Stability | PASS/FAIL | |
| Test 9: Performance | PASS/FAIL | |
| Test 10: Config | PASS/FAIL | |

### Issues Found

1. [Issue description]
2. [Issue description]

### Overall Assessment

PASS / FAIL / PARTIAL

### Recommendations

- [Next steps]
```
