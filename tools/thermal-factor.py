#!/usr/bin/env python3
"""
thermal-factor.py - convert a raw temp-vs-nits measurement log into a thermal
                    compensation factor table that als-dimmer can use to
                    correct brightness-to-nits readings for backlight
                    temperature drift.

The brightness-to-nits LUT (produced by als-dimmer-sweep.py) captures the
panel's response at the temperature the sweep happened to be running at.
Once the panel reaches steady-state thermal equilibrium (typically 10-30
minutes after power-on), LED junction efficiency and phosphor quantum yield
both drop, and the same brightness % produces 4-8% fewer nits. This script
turns a "let the panel run and log nits + temperature every 30 seconds"
measurement into a small CSV the daemon can use to undo that drift.

Input format (raw measurement log, e.g. from your live-measurements-child.py):

  # comments
  sample_index,timestamp,elapsed_seconds,X,Y,Z,x,y,backlight_temp_c,
  als_absolute_nits,als_brightness_pct,delta_nits,delta_pct
  1,...,1175.5,...,37.80,...,100,...
  2,...,1185.8,...,40.60,...,100,...
  ...

Only rows with `als_brightness_pct == 100` are used (the factor must be
derived at a fixed brightness so the only varying input is temperature).

Output format (consumed by als-dimmer's thermal_compensation block):

  # label=...
  # reference_temp_c=38.0
  # source=...
  backlight_temp_c,factor
  38.0,1.0000
  40.0,0.9943
  ...
  55.0,0.9554

`factor` is the multiplier the daemon applies to the LUT-predicted nits to
get the actual displayed nits at that temperature. By definition,
factor(reference_temp_c) == 1.0.

Examples:

  # Auto-pick the coldest measured temp as the reference
  ./thermal-factor.py \\
      --input  calibrations/dimmer_12_3_nq1v1_temp_nits_relation.csv \\
      --output calibrations/dimmer_12_3_nq1v1_thermal_factor.csv \\
      --label  dimmer_12_3_nq1v1_warm

  # Explicit reference: use 38 C (typical sweep-time temperature)
  ./thermal-factor.py --input raw.csv --output factor.csv --reference-temp 38.0

  # Tighter binning + heavier smoothing for noisy data
  ./thermal-factor.py --input raw.csv --output factor.csv \\
      --bin-size 0.2 --smooth-window 9
"""

import argparse
import csv
import datetime
import sys
from pathlib import Path


# Required column names in the input CSV. We're tolerant of additional
# columns; only these three are read.
_INPUT_COLS = ("backlight_temp_c", "Y", "als_brightness_pct")


def parse_raw_measurements(path):
    """Parse the raw temp-vs-nits log. Returns a list of (temp_c, nits_y)
    tuples for rows where als_brightness_pct == 100. Skips comment lines
    starting with '#' and any rows that fail to parse."""
    rows = []
    with open(path, newline="") as f:
        # Strip comment lines BEFORE giving the file to csv.DictReader so
        # the first non-comment line is taken as the header.
        non_comment = [line for line in f if not line.lstrip().startswith("#")]
    reader = csv.DictReader(non_comment)
    missing = [c for c in _INPUT_COLS if c not in (reader.fieldnames or [])]
    if missing:
        raise RuntimeError(
            f"input CSV missing required column(s): {', '.join(missing)}; "
            f"got headers: {reader.fieldnames}"
        )
    for r in reader:
        try:
            if float(r["als_brightness_pct"]) != 100.0:
                continue
            temp = float(r["backlight_temp_c"])
            nits = float(r["Y"])
        except (ValueError, KeyError):
            continue
        if nits <= 0:
            continue
        rows.append((temp, nits))
    return rows


def bin_by_temperature(rows, bin_size):
    """Group measurements by temperature bin (rounded to nearest bin_size)
    and average. Returns a list sorted by temperature. Handles repeat
    measurements at the same temp during steady-state."""
    if bin_size <= 0:
        raise ValueError("bin_size must be > 0")
    bins = {}
    for temp, nits in rows:
        key = round(temp / bin_size) * bin_size
        bins.setdefault(key, []).append(nits)
    return sorted((round(t, 4), sum(v) / len(v)) for t, v in bins.items())


def smooth_rolling_mean(rows, window):
    """Symmetric rolling mean over an odd window. The window is clipped at
    the edges, so the first and last few entries are averaged over fewer
    samples. Length-preserving."""
    if window <= 1 or len(rows) <= 1:
        return list(rows)
    half = window // 2
    smoothed = []
    for i in range(len(rows)):
        lo = max(0, i - half)
        hi = min(len(rows), i + half + 1)
        avg = sum(r[1] for r in rows[lo:hi]) / (hi - lo)
        smoothed.append((rows[i][0], avg))
    return smoothed


def interpolate_at_temp(rows, target_temp):
    """Linear-interpolate the second column at target_temp. Caller has
    already sorted rows by temp ascending. Returns None if rows is empty."""
    if not rows:
        return None
    if target_temp <= rows[0][0]:
        return rows[0][1]
    if target_temp >= rows[-1][0]:
        return rows[-1][1]
    for i in range(len(rows) - 1):
        t0, v0 = rows[i]
        t1, v1 = rows[i + 1]
        if t0 <= target_temp <= t1:
            if t1 == t0:
                return (v0 + v1) / 2.0
            return v0 + (target_temp - t0) / (t1 - t0) * (v1 - v0)
    return rows[-1][1]


def compute_factors(rows, ref_temp_c):
    """Convert a list of (temp, nits) into (temp, factor) where
    factor = nits / nits_at_ref_temp. Caller has already binned/smoothed."""
    ref_nits = interpolate_at_temp(rows, ref_temp_c)
    if ref_nits is None or ref_nits <= 0:
        return None
    return [(t, n / ref_nits) for t, n in rows]


def write_factor_csv(out_path, factors, ref_temp_c, args, source_basename, raw_count):
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as f:
        f.write("# als-dimmer thermal compensation factor table\n")
        if args.label:
            f.write(f"# label={args.label}\n")
        f.write(f"# reference_temp_c={ref_temp_c:.2f}\n")
        f.write(f"# source={source_basename}\n")
        f.write(f"# generated_at={datetime.datetime.now().astimezone().isoformat()}\n")
        f.write(f"# bin_size_c={args.bin_size} smooth_window={args.smooth_window}\n")
        f.write(f"# raw_rows={raw_count} factor_rows={len(factors)}\n")
        if factors:
            f.write(f"# factor_min={min(p[1] for p in factors):.6f} "
                    f"factor_max={max(p[1] for p in factors):.6f}\n")
        f.write("backlight_temp_c,factor\n")
        for t, fac in factors:
            f.write(f"{t:.2f},{fac:.6f}\n")


def main():
    ap = argparse.ArgumentParser(
        description="Convert a temp-vs-nits log into an als-dimmer thermal "
                    "compensation factor table.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:", 1)[-1] if "Examples:" in __doc__ else "",
    )
    ap.add_argument("--input", required=True, type=Path,
                    help="Raw measurement CSV (must have columns "
                         "als_brightness_pct, backlight_temp_c, Y)")
    ap.add_argument("--output", required=True, type=Path,
                    help="Path to write the factor CSV (parent dirs auto-created)")
    ap.add_argument("--reference-temp", type=float, default=None,
                    help="Temperature in degC at which the brightness sweep was "
                         "taken. The factor at this temperature is 1.0. If "
                         "omitted, uses the coldest measured temp.")
    ap.add_argument("--bin-size", type=float, default=0.5,
                    help="Bin width in degC. Measurements within one bin are "
                         "averaged together (default: 0.5)")
    ap.add_argument("--smooth-window", type=int, default=5,
                    help="Rolling-mean window over the binned data (odd "
                         "values centred; default: 5)")
    ap.add_argument("--label", default="",
                    help="Free-form label written into the CSV header")
    args = ap.parse_args()

    if not args.input.exists():
        print(f"error: input file not found: {args.input}", file=sys.stderr)
        return 1

    try:
        raw = parse_raw_measurements(args.input)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if not raw:
        print("error: no usable rows (need als_brightness_pct=100 and "
              "valid backlight_temp_c + Y)", file=sys.stderr)
        return 1

    binned = bin_by_temperature(raw, args.bin_size)
    if len(binned) < 2:
        print("error: need at least 2 distinct temperature bins to derive "
              "a factor table; got only %d" % len(binned), file=sys.stderr)
        return 1

    smoothed = smooth_rolling_mean(binned, args.smooth_window)

    ref_temp = args.reference_temp
    if ref_temp is None:
        ref_temp = smoothed[0][0]
        print(f"info: --reference-temp not specified, using coldest measured "
              f"temp ({ref_temp:.2f} C) as the reference", file=sys.stderr)
    else:
        # Warn if user-supplied reference is outside the measured range.
        t_min, t_max = smoothed[0][0], smoothed[-1][0]
        if ref_temp < t_min or ref_temp > t_max:
            print(f"warning: --reference-temp {ref_temp:.2f} is outside the "
                  f"measured range [{t_min:.2f}, {t_max:.2f}] - factors will "
                  f"be extrapolated", file=sys.stderr)

    factors = compute_factors(smoothed, ref_temp)
    if not factors:
        print("error: could not compute factor (reference nits resolved to "
              "zero/negative)", file=sys.stderr)
        return 1

    write_factor_csv(args.output, factors, ref_temp, args,
                     args.input.name, len(raw))

    fmin = min(f for _, f in factors)
    fmax = max(f for _, f in factors)
    print(f"wrote {len(factors)} factor rows to {args.output}")
    print(f"  reference_temp_c = {ref_temp:.2f}")
    print(f"  factor range     = {fmin:.4f} .. {fmax:.4f} "
          f"(implied delta {(fmax - fmin) * 100:.2f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
