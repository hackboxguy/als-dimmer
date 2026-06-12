# OPT4001 I2C Access Reference

This note documents the proven I2C transaction sequence for using a TI
OPT4001 ambient light sensor from an arbitrary I2C master. It was prepared by
comparing two known-good implementations in this workspace:

- `Esp32-CAN-ALS/main/opt4001_driver.c`: ESP32 I2C master.
- `als-dimmer/src/sensors/opti4001_sensor.cpp`: Linux userspace I2C master
  using the native `/dev/i2c-*` interface.

Both implementations follow the same functional mechanism:

1. Read and verify device ID register `0x11`.
2. Write configuration register `0x0A = 0x3239`.
3. Wait for at least one conversion.
4. Let the OPT4001 hardware auto-range internally.
5. Read result registers `0x00` and `0x01`.
6. Decode `EXPONENT`, 20-bit `MANTISSA`, and convert to lux.

The ESP32 and Linux code differ only in transport details and timing margin:
the ESP32 uses repeated-start 16-bit reads and waits 900 ms after
configuration, while `als-dimmer` uses Linux `write()` then `read()` 16-bit
transactions and waits 150 ms. Both are valid for the current `0x3239`
configuration because conversion time field `0x8` is 100 ms, not 800 ms.

## Device Basics

- 7-bit I2C address used by these boards: `0x44`.
- Address byte for address `0x44`: write `0x88`, read `0x89`.
- Register payloads are 16-bit and transmitted most-significant byte first.
- Standard mode 100 kHz and fast mode 400 kHz are supported. The datasheet
  also defines high-speed I2C operation, but the two known-good
  implementations use ordinary 100 kHz class transfers.
- SOT-5X3 package boards can select the I2C address with the `ADDR` pin.
  PicoStar package devices have a fixed `0x45` address.

Do not use host APIs that silently byte-swap 16-bit SMBus words unless you
explicitly correct the endianness. Reading raw bytes and assembling
`(msb << 8) | lsb` is unambiguous.

## Important Registers

| Register | Width | Access | Meaning |
| --- | --- | --- | --- |
| `0x00` | 16 bit | R | `EXPONENT[15:12]`, `RESULT_MSB[11:0]` |
| `0x01` | 16 bit | R | `RESULT_LSB[15:8]`, `COUNTER[7:4]`, `CRC[3:0]` |
| `0x0A` | 16 bit | R/W | Main configuration |
| `0x0B` | 16 bit | R/W | Interrupt direction/config and `I2C_BURST` |
| `0x0C` | 16 bit | mixed | Flags, including overload and conversion-ready |
| `0x11` | 16 bit | R | Device ID. Bits `11:0` are `0x121` for OPT4001 |

## I2C Transactions

### Write One 16-bit Register

For example, writing configuration register `0x0A = 0x3239`:

```text
START
send 0x88             ; target address 0x44 + write bit
expect ACK
send 0x0A             ; register pointer
expect ACK
send 0x32             ; data MSB
expect ACK
send 0x39             ; data LSB
expect ACK
STOP
```

### Read One 16-bit Register

The sensor read pointer is set by a write phase. A repeated START is preferred
when the master supports it, but a STOP between the pointer write and the read
also works and is what the Linux `write()` plus `read()` implementation does.

```text
START
send 0x88             ; target address 0x44 + write bit
expect ACK
send register         ; for example 0x00
expect ACK
REPEATED START        ; or STOP, then START
send 0x89             ; target address 0x44 + read bit
expect ACK
read data_msb
send ACK
read data_lsb
send NACK
STOP
```

### Preferred Result Read

For a generic I2C master implementation, prefer reading the two result
registers in one burst from register `0x00`. This minimizes bus overhead and
reduces the chance of reading `0x00` from one conversion and `0x01` from the
next.

The datasheet default for register `0x0B` enables `I2C_BURST`. If firmware ever
writes `0x0B`, preserve its required bits and keep `I2C_BURST=1`.

```text
START
send 0x88
expect ACK
send 0x00             ; result register 0 pointer
expect ACK
REPEATED START
send 0x89
expect ACK
read b0               ; register 0x00 MSB
send ACK
read b1               ; register 0x00 LSB
send ACK
read b2               ; register 0x01 MSB
send ACK
read b3               ; register 0x01 LSB
send NACK
STOP
```

Then:

```c
uint16_t reg0 = ((uint16_t)b0 << 8) | b1;
uint16_t reg1 = ((uint16_t)b2 << 8) | b3;
```

The two repository implementations read `0x00` and `0x01` as separate 16-bit
transactions, and that has been validated against an external lux meter. A
single 4-byte burst is the cleaner generic-master pattern when burst read is
available.

## Initialization Sequence

Use this sequence after power-up or after an I2C/general reset:

1. Bring up the I2C bus and select 7-bit address `0x44`, or the address
   selected by the board package/pin strap.
2. Read register `0x11`.
3. Check `(reg11 & 0x0FFF) == 0x0121`.
4. Write register `0x0A = 0x3239`.
5. Read register `0x0A` back and require `0x3239`.
6. Wait at least 100 ms plus margin. The known-good Linux implementation uses
   150 ms. A 900 ms wait is also safe but unnecessarily slow for `0x3239`.
7. Begin the periodic result-read loop.

For any master implemented as explicit states, a simple sequence is:

```text
POWER_WAIT
READ_ID
CHECK_ID
WRITE_CONFIG_0A_3239
READBACK_CONFIG
WAIT_FIRST_CONVERSION
READ_RESULT_BURST
DECODE_LUX
WAIT_NEXT_SAMPLE
```

If any address, register, or data byte is NACKed, abort the current
transaction, issue STOP, wait briefly, and retry from a known state. Treat a
bad device ID or wrong configuration readback as a hard bring-up failure.

## Configuration Register `0x0A`

The working value is:

```text
0x3239 = 0011 0010 0011 1001b
```

| Bits | Field | Value | Meaning |
| --- | --- | --- | --- |
| `15` | `QWAKE` | `0` | Normal continuous operation |
| `14` | reserved | `0` | Must read/write zero |
| `13:10` | `RANGE` | `0xC` | Automatic full-scale range |
| `9:6` | `CONVERSION_TIME` | `0x8` | 100 ms conversion time |
| `5:4` | `OPERATING_MODE` | `0x3` | Continuous conversion |
| `3` | `LATCH` | `1` | Latched threshold/interrupt behavior |
| `2` | `INT_POL` | `0` | Active-low interrupt polarity |
| `1:0` | `FAULT_COUNT` | `0x1` | Two fault counts |

`RANGE=0xC` is the key auto-range setting. Once this is configured, the I2C
master must not run a manual gain/range loop. The sensor chooses the internal
full-scale range and reports the selected range as `EXPONENT` in the result.

The common bad value to avoid is `0xC839`. It sets `QWAKE=1`, violates reserved
bit 14, and leaves `CONVERSION_TIME=0` for 600 us. In earlier `als-dimmer`
testing that value caused the device to reject the intended auto-range setup
and saturate under bright light. `0x3239` is the corrected value used by both
known-good repositories.

## Reading And Converting Lux

Result register layout:

```text
reg0 = register 0x00
  bits 15:12  EXPONENT
  bits 11:0   RESULT_MSB

reg1 = register 0x01
  bits 15:8   RESULT_LSB
  bits 7:4    COUNTER
  bits 3:0    CRC
```

Decode:

```c
uint8_t exponent = (reg0 >> 12) & 0x0F;
uint32_t result_msb = reg0 & 0x0FFF;
uint32_t result_lsb = (reg1 >> 8) & 0xFF;
uint8_t counter = (reg1 >> 4) & 0x0F;
uint8_t crc = reg1 & 0x0F;

if (exponent > 8) {
    /* Invalid for normal OPT4001 result data. Discard and retry. */
    return false;
}

uint32_t mantissa = (result_msb << 8) | result_lsb;  // 20 bits
uint32_t adc_codes = mantissa << exponent;           // up to 28 bits
float lux_sot5x3 = (float)adc_codes * 437.5e-6f;
float lux_picostar = (float)adc_codes * 312.5e-6f;
```

Use the package-specific scale factor:

- SOT-5X3: `lux = ADC_CODES * 437.5e-6`.
- PicoStar: `lux = ADC_CODES * 312.5e-6`.

The board used by the two repository implementations uses the SOT-5X3 factor,
so both compute lux with `437.5e-6` or `0.0004375`.

Normal OPT4001 result exponents are `0..8`, matching the 9 full-scale ranges.
Use at least 32-bit unsigned arithmetic for `mantissa` and `adc_codes` after
checking the exponent. Using 64-bit arithmetic in test or verification code is
fine, but the actual OPT4001 ADC code range fits in 28 bits.

## Fresh-Sample And Error Checks

Minimum checks for a simple master:

- Confirm the register `0x0A` readback is exactly `0x3239`.
- Confirm the sample counter in register `0x01[7:4]` changes over time.
- Clamp or flag lux only after conversion; do not reject samples merely because
  the exponent changed. Exponent changes are normal auto-range behavior.

Recommended checks for a robust I2C master:

- Read `0x00` and `0x01` in one 4-byte burst.
- Use `COUNTER` to detect repeated samples when polling faster than the
  conversion time. It is a 4-bit counter and wraps from 15 to 0.
- Optionally validate `CRC[3:0]` using the datasheet CRC equations.
- Optionally read flags register `0x0C`. Bit 3 is overload, bit 2 is
  conversion-ready. Reading `0x0C` clears the conversion-ready flag and, in
  latched mode, clears latched threshold flags, so do not poll it casually if
  the interrupt/threshold subsystem is also in use.

Polling faster than 100 ms with `CONVERSION_TIME=0x8` can return the same
sample again. That is not an I2C error.

During a fast increasing light transient, auto-range can take longer than the
nominal conversion time because the device may abort an over-range measurement,
increase range, and retake the measurement. The master should tolerate missing
or repeated counters during such transients instead of trying to force a manual
range.

## Repository Comparison

| Topic | ESP32 implementation | `als-dimmer` native I2C implementation | Verdict |
| --- | --- | --- | --- |
| I2C address | Fixed `0x44` | Configured as `"0x44"` in OPT4001 configs | Same board-level address |
| Device ID | Reads `0x11`, requires `0x121` | Reads `0x11`, warns if not `0x121` | Same read. A strict master should fail hard on mismatch |
| Configuration | Writes `0x3239` | Writes `0x3239` | Same correct auto-range config |
| Conversion time | Code writes field `0x8`; comments say 800 ms | Code and comments identify `0x8` as 100 ms | Behavior same; ESP32 comment is stale |
| First wait | 900 ms | 150 ms | Both safe; 150 ms is enough for `0x8` |
| Result read | Reads `0x00`, then `0x01`, each MSB-first with repeated START | Reads `0x00`, then `0x01`, each MSB-first via `write()`/`read()` | Same logical register access |
| Auto-range management | Lets hardware manage range | Lets hardware manage range | Same correct mechanism |
| Lux math | 20-bit mantissa, exponent shift, SOT factor | 20-bit mantissa, exponent shift, SOT factor | Same correct conversion |

The practical conclusion is that both repositories implement the same correct
OPT4001 mechanism. The important reference value for any master is `0x3239`,
and the important readout rule is to use both result registers together:
`EXPONENT` from `0x00[15:12]`, `RESULT_MSB` from `0x00[11:0]`, and
`RESULT_LSB` from `0x01[15:8]`.

## Generic I2C Master Checklist

- Generate normal I2C START, repeated START, STOP, ACK, and final NACK.
- Use 7-bit address `0x44` unless the board strap/package says otherwise.
- Transmit and receive register payloads MSB first.
- On initialization, read `0x11` and check lower 12 bits for `0x121`.
- Write `0x0A` with bytes `0x32, 0x39`.
- Read back `0x0A` and require `0x3239`.
- Wait at least 150 ms before the first result read.
- Prefer a 4-byte burst read from `0x00`; otherwise read `0x00` and `0x01`
  back-to-back.
- Decode the 20-bit mantissa and 4-bit exponent exactly as shown above.
- Use the SOT-5X3 lux factor `437.5e-6` for the existing board.
- Do not write manual range values during normal operation.
- Do not set reserved bit 14 or `QWAKE` in continuous mode.
- Treat counter changes, optional CRC, optional overload flag, and I2C ACK
  status as diagnostics around the same core lux calculation.

## References

- TI, `OPT4001 High Speed, High Precision, Digital Ambient Light Sensor`
  datasheet, SBOS993A, Rev. A:
  https://www.ti.com/lit/ds/symlink/opt4001.pdf. Local split copies are stored
  as `docs/opt4001_part1.pdf` through `docs/opt4001_part5.pdf`.
- Local root-cause note: `docs/OPT4001_BUG_ANALYSIS.md`.
- ESP32 source: `../Esp32-CAN-ALS/main/opt4001_driver.c`.
- Linux native I2C source: `src/sensors/opti4001_sensor.cpp`.
