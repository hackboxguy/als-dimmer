# FPGA OPT4001 Reader Improvement Plan

This note documents the `als-dimmer` side of the planned FPGA OPT4001 reader
fix described in
`../../pixelpipe-fpga/docs/opti4001-reader-improvement-plan.md`.
The key integration question is whether the fixed FPGA can expose the full
OPT4001 light range to `als-dimmer`, and whether the existing
`fpga_opti4001` reader needs to change.

## Short Answer

Yes. If the FPGA RTL fixes the transaction and decode issues in the improvement
plan, the FPGA should be able to read and expose the normal SOT-5X3 OPT4001
range, including practical automotive values around `80k..100k lux`.

The proof point on hardware is the OPT4001 result exponent. A design stuck near
`12k lux` is probably not reaching exponent 6 and above. With correct
`0x3239` configuration, coherent result reads, and clean sample capture, bright
light should move the decoded exponent through the upper ranges and produce
lux values well beyond 12k.

The `als-dimmer` software should support a new fixed-RTL FPGA sensor type
instead of changing the existing `fpga_opti4001` behavior in place. Keep
`fpga_opti4001` as the legacy `00 + raw24 * scale` contract, and add
`fpga_opti4001_lux` for the new polished-lux FPGA contract.

## Current `als-dimmer` Behavior

The native FPGA I2C reader is `src/sensors/fpga_opti4001_sensor.cpp`.

Current protocol assumption:

- Linux master talks to FPGA I2C-slave address `0x1D`.
- It writes four bytes: `0x00 0x00 0x00 0x0C`.
- It reads four bytes back.
- It treats byte 0 as reserved.
- It combines bytes 1..3 as a 24-bit big-endian value.
- It returns `lux = raw24 * scale_factor`.

Relevant code:

- `src/sensors/fpga_opti4001_sensor.cpp:18-25` documents the old protocol.
- `src/sensors/fpga_opti4001_sensor.cpp:82-92` performs the register read.
- `src/sensors/fpga_opti4001_sensor.cpp:105-113` ignores byte 0, builds
  `raw_value` from bytes 1..3, and applies `scale_factor_`.
- `include/als-dimmer/config.hpp:23-24` defaults `scale_factor` to `0.64`.
- `configs/config_fpga_opti4001_dimmer2048.json:5-10` sets
  `scale_factor` to `1.64`.

This means the current reader is not a transparent "read lux from FPGA" path.
It is a "read legacy FPGA raw value and scale it" path.

## Why A Software Change Is Needed

The proposed RTL fix should make the FPGA compute or expose a correct cached
lux value from:

```text
mantissa = {reg0[11:0], reg1[15:8]}
adc_codes = mantissa << exponent
lux_sot5x3 = adc_codes * 0.0004375
```

If the fixed FPGA exposes integer lux at register `0x0C`, the existing
`als-dimmer` reader would multiply that lux value again by `scale_factor`.
With the current `dimmer2048` configs, that means:

```text
actual 50,000 lux -> als-dimmer reports 82,000 lux
actual 61,000 lux -> als-dimmer reports ~100,000 lux
actual 80,000 lux -> als-dimmer reports 131,200 lux
```

The daemon only warns above about `120k lux`; it does not reject the value.
The zone mapper then clamps values above the last configured zone maximum, so
brightness would reach the top of the outdoor curve too early.

## Recommended Host Contract

Preferred contract for the fixed FPGA:

```text
Host read register 0x0C, length 4
Response: uint32 big-endian cached lux
Unit: integer lux
Valid range: 0..117441 lux for the existing SOT-5X3 scale
Error sentinel: 0xffffffff
```

Why integer lux is the best fit:

- It is enough resolution for dimming decisions.
- It fits in 17 bits for `0..117441 lux`, so the old reader's byte-0-ignore
  behavior still happens to work while values stay below `0x01000000`.
- It avoids host-side OPT4001 package math and duplicate scaling.
- It is easy to test manually with `i2ctransfer`.

Optional but better long-term contract:

- Keep `0x0C` as cached integer lux.
- Add status/debug registers for raw `reg0`, raw `reg1`, exponent, counter,
  CRC status, config-ok, sample-valid, I2C error, and retry counters.
- Add a version or capability register so `als-dimmer` can tell whether the
  FPGA is exposing legacy raw24, integer lux, or milli-lux.

Avoid exposing milli-lux at `0x0C` unless the software reader is changed first.
Milli-lux can reach about `117,441,000`, which no longer fits in the current
reader's 24-bit extraction path.

## Recommended `als-dimmer` Migration

Use two explicit sensor types:

| Sensor type | FPGA register contract | Intended bitstream |
| --- | --- | --- |
| `fpga_opti4001` | `0x0C -> 00 + raw24`; `lux = raw24 * scale_factor` | Existing/legacy RTL |
| `fpga_opti4001_lux` | `0x0C -> uint32 big-endian integer lux`; `scale_factor = 1.0` | Fixed full-range RTL |

This avoids a hidden behavior change. Existing deployments keep their old
configs and old scaling. New fixed-RTL deployments opt in by selecting a new
sensor type whose name describes the register format.

### New Sensor Module

Add a new module, for example:

```text
src/sensors/fpga_opti4001_lux_sensor.cpp
```

The new reader can reuse the same Linux I2C transaction as the legacy reader:

```text
write 0x00 0x00 0x00 0x0C
read 4 bytes
```

But it should decode the response as full 32-bit big-endian integer lux:

```cpp
uint32_t u32 = (uint32_t(buf[0]) << 24) |
               (uint32_t(buf[1]) << 16) |
               (uint32_t(buf[2]) << 8)  |
               uint32_t(buf[3]);

if (u32 == 0xffffffffu) {
    return -1.0f;
}

float lux = static_cast<float>(u32);
```

Recommended implementation details:

- Return sensor type string `fpga_opti4001_lux`.
- Treat `0xffffffff` as the FPGA/sensor error sentinel.
- Warn, but do not immediately fail, above the SOT-5X3 expected region
  (`~117k lux`).
- Do not multiply by the legacy `scale_factor`; if the config parser still
  passes a scale value, keep it at `1.0` for this sensor type.
- Prefer factoring the common I2C read transaction into a helper only if the
  duplication becomes annoying. The important behavior difference is decode,
  not transport.

### Factory And Config Validation

Update these integration points:

- `include/als-dimmer/config.hpp`: add `fpga_opti4001_lux` to the sensor type
  comment and make clear that its default scale is `1.0`.
- `src/config.cpp`: accept `fpga_opti4001_lux` as an I2C sensor type requiring
  `device` and `address`.
- `src/main.cpp`: add a factory declaration and create the new sensor when
  `sensor.type == "fpga_opti4001_lux"`.
- `docs/CLAUDE.md` and any user docs: document both legacy and lux contracts.

### New Config Set

Do not change the legacy configs for old bitstreams. Create parallel configs
for the fixed RTL, for example:

- `configs/config_fpga_opti4001_lux_dimmer2048.json`
- `configs/config_fpga_opti4001_lux_dimmer2048_12_3_nq1v1.json`
- `configs/config_fpga_opti4001_lux_dimmer2048_15_6_0od.json`
- `configs/config_fpga_opti4001_lux_dimmer800.json`
- `configs/config_fpga_opti4001_lux_dimmer200.json`
- `configs/config_fpga_opti4001_lux_ddcutil.json`

The sensor block should look like:

```json
"sensor": {
  "type": "fpga_opti4001_lux",
  "device": "/dev/i2c-1",
  "address": "0x1D",
  "scale_factor": 1.0
}
```

### Legacy Configs To Leave Alone

Keep these on the old `fpga_opti4001` reader unless they are explicitly moved
to a fixed RTL bitstream:

- `configs/config_fpga_opti4001_dimmer2048.json`
- `configs/config_fpga_opti4001_dimmer2048_12_3_nq1v1.json`
- `configs/config_fpga_opti4001_dimmer2048_15_6_0od.json`
- `configs/config_fpga_opti4001_dimmer800.json`
- `configs/config_fpga_opti4001_dimmer200.json`
- `configs/config_fpga_opti4001_ddcutil.json`
- `configs/config_fpga_sysfs.json`

The `dimmer2048`, panel-specific `dimmer2048`, `dimmer800`, and sysfs configs
currently set `scale_factor: 1.64`. That value should not be used with a fixed
FPGA that already reports lux, but it can remain in configs tied to legacy RTL.

## Zone Mapping Impact

The existing zones are already compatible with a full-range OPT4001:

```json
{ "name": "outdoor", "lux_range": [500, 100000] }
```

`src/zone_mapper.cpp` selects the last zone for out-of-range high lux and then
clamps lux to the zone's max during curve evaluation. Therefore no zone-mapper
code change is required for `80k..100k lux`.

However, once the FPGA exposes correct full-range lux, the display will spend
more time in the upper part of the outdoor curve instead of plateauing near the
old `12k lux` ceiling. That is expected. Re-tune the outdoor curve only after
the sensor reading is verified against an external lux meter.

## Validation Plan

After the RTL fix and software contract update:

1. Read the FPGA register directly:

```bash
i2ctransfer -y 1 w4@0x1D 0x00 0x00 0x00 0x0C r4
```

2. Confirm the decoded value matches the selected `als-dimmer` sensor type:

- `fpga_opti4001`: bytes 1..3 are legacy raw24 and must still be multiplied by
  the configured scale factor.
- `fpga_opti4001_lux`: all four bytes are unsigned big-endian integer lux and
  should be reported directly.

3. Compare `als-dimmer` `GET_LUX` against:

- external lux meter
- FPGA ILA/debug exponent and raw result registers
- native Linux `opti4001` reader on the same sensor, when available

4. Test bright values around:

```text
1k, 10k, 12k, 20k, 50k, 80k, 100k lux
```

Expected result:

- FPGA internal exponent rises above 5 in bright light.
- FPGA cached lux continues above 12k.
- `als-dimmer` reports the same lux scale, without an extra `1.64` or `0.64`
  multiplier.
- The outdoor zone reaches 100 percent only near the configured upper range,
  not early because of double scaling.

## Implementation Recommendation

For the first fixed RTL deployment, use this conservative migration:

1. Keep the old `fpga_opti4001` reader unchanged as the legacy raw24/scaled
   path so older bitstreams keep working.
2. Add `fpga_opti4001_lux` as a separate sensor type.
3. Create fixed-RTL configs with:

```json
"type": "fpga_opti4001_lux",
"scale_factor": 1.0
```

4. Keep old configs on `type: "fpga_opti4001"` until they are intentionally
   paired with fixed RTL.
5. Update `docs/CLAUDE.md` and any register-map notes that still describe
   `raw * 0.64` as the only FPGA OPT4001 format.

This gives a clean path for old and new FPGA bitstreams to coexist while making
the full-range OPT4001 behavior visible to `als-dimmer`.
