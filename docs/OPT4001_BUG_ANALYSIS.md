# OPT4001 Auto-Range Failure - Root Cause Analysis

## Problem Summary
The OPT4001 sensor on Raspberry Pi was rejecting auto-range mode and saturating at 1,834 lux when exposed to 70k-85k lux light. The ESP32 reference implementation worked correctly for the same sensor.

## Root Cause
The als-dimmer code had **three critical configuration bugs**:

### Bug 1: Reserved Bit Violation (CRITICAL)
- **Configuration written**: 0xC839
- **Bit 14 (Reserved)**: Set to 1
- **Datasheet requirement**: "Must read or write 0"
- **Hardware behavior**: Hardware cleared bit 14 **AND** bit 13 as protective measure
- **Result**: Auto-range (RANGE=12) changed to fixed Range 8 (117.4klux max)

### Bug 2: QWAKE Bit Incorrectly Set
- **Bit 15 (QWAKE)**: Set to 1
- **Should be**: 0 (normal operation)
- **Effect**: May have contributed to configuration rejection

### Bug 3: Wrong Conversion Time
- **Value used**: 0 (600µs)
- **Should be**: 8 (100ms) to match working ESP32
- **Effect**: Insufficient time for proper sensor operation

## Configuration Breakdown

### Incorrect als-dimmer config (0xC839):
```
Bit 15 (QWAKE):          1 ✗ Should be 0
Bit 14 (Reserved):       1 ✗ VIOLATION - Must be 0!
Bits 13-10 (RANGE):      1100 = 12 (auto-range) ✓
Bits 9-6 (CONV_TIME):    0000 = 0 (600µs) ✗ Too short!
Bits 5-4 (MODE):         11 = 3 (continuous) ✓
Bit 3 (LATCH):           1 ✓
Bit 2 (INT_POL):         0 ✓
Bits 1-0 (FAULT_COUNT):  01 = 1 ✓
```

### Hardware readback (0x8839):
```
Bit 15 (QWAKE):          1 (kept from write)
Bit 14 (Reserved):       0 (cleared by hardware)
Bits 13-10 (RANGE):      1000 = 8 ← Changed from 12!
Bits 9-6 (CONV_TIME):    0000 = 0
Bits 5-4 (MODE):         11 = 3
Bit 3 (LATCH):           1
Bit 2 (INT_POL):         0
Bits 1-0 (FAULT_COUNT):  01 = 1
```

### Correct ESP32 config (0x3239):
```
Bit 15 (QWAKE):          0 ✓
Bit 14 (Reserved):       0 ✓
Bits 13-10 (RANGE):      1100 = 12 (auto-range) ✓
Bits 9-6 (CONV_TIME):    1000 = 8 (100ms) ✓
Bits 5-4 (MODE):         11 = 3 (continuous) ✓
Bit 3 (LATCH):           1 ✓
Bit 2 (INT_POL):         0 ✓
Bits 1-0 (FAULT_COUNT):  01 = 1 ✓
```

## OPT4001 RANGE Values (SOT-5X3 Package)

| RANGE | Full-Scale Lux | Notes                          |
|-------|----------------|--------------------------------|
| 0     | 459            |                                |
| 1     | 918            |                                |
| 2     | 1,835          | Where it was stuck before fix  |
| 3     | 3,670          |                                |
| 4     | 7,340          |                                |
| 5     | 14,680         |                                |
| 6     | 29,360         |                                |
| 7     | 58,700         |                                |
| 8     | 117,400        | ESP32 "jump" at 57k→117k       |
| 9-11  | Higher         | Not documented in excerpt      |
| 12    | Auto-range     | Hardware manages 0-11          |

## Conversion Time Values

| Value | Time   | Notes                           |
|-------|--------|---------------------------------|
| 0     | 600µs  | Too short for reliable operation|
| 8     | 100ms  | Used by working ESP32 code      |
| 11    | 800ms  | Highest accuracy (not needed)   |

## Fix Applied
Changed configuration from `0xC839` to `0x3239` to match working ESP32:
- Cleared QWAKE bit (15)
- Cleared reserved bit (14) 
- Set CONV_TIME to 8 (100ms)
- Adjusted wait time from 900ms to 150ms

## Expected Results
- Auto-range should now be accepted by hardware
- Sensor should properly scale from 0 to maximum lux
- Configuration readback should match written value (0x3239)
- Target range: 0-57k lux minimum (matching ESP32 performance)

## Additional Notes
Both the ESP32 and original als-dimmer code had misleading comments claiming "800ms conversion time" when actually using 100ms. The datasheet shows:
- Value 8 = 100ms (what both codes use)
- Value 11 = 800ms (actual 800ms)

This was documentation-only error and doesn't affect functionality.
