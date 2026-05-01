#!/usr/bin/env python3
"""
blend-calibrations.py - average two or more als-dimmer calibration CSVs into
                        a single "neutral" table that minimizes worst-case
                        error across the inputs.

When you have calibration data from multiple panel variants but want a single
default config that works passably for any of them, the right answer is
typically a per-row arithmetic mean of the inputs. The result has half the
panel-to-panel error each input would produce vs the truth - if panel A reads
1226 nits at 100% and panel B reads 1106, the neutral table reports 1166 and
each panel ends up ~5% off (half the original 10% gap) instead of one panel
being ~10% off when the other panel's specific table is used.

Auto-detects format from the column header:
  - "brightness_pct,nits,..."        - brightness-to-nits LUT (assumes
                                        identical brightness_pct rows in
                                        all inputs)
  - "backlight_temp_c,factor,..."    - thermal compensation factor LUT
                                        (renormalized to a common reference
                                        temperature, then per-temp average)

For thermal factor blending, inputs may have different temperature ranges and
different reference temps. The script picks the union of all input
temperature points (with 0.5 C bin alignment), renormalizes each input to a
common --target-reference-temp (default = the median reference temp across
inputs), interpolates each input's renormalized factor at the union temps,
and averages whichever inputs have data at each temp.

Examples:
  # Blend the brightness LUTs of the 12.3" and 15.6" panels
  ./blend-calibrations.py \\
      --input  calibrations/dimmer_12_3_nq1v1.csv \\
      --input  calibrations/dimmer_15_6_0od.csv \\
      --output calibrations/dimmer_neutral.csv \\
      --label  neutral

  # Same for the thermal factors
  ./blend-calibrations.py \\
      --input  calibrations/dimmer_12_3_nq1v1_thermal_factor.csv \\
      --input  calibrations/dimmer_15_6_0od_thermal_factor.csv \\
      --output calibrations/dimmer_neutral_thermal_factor.csv \\
      --target-reference-temp 38.5 \\
      --label  neutral
"""

import argparse
import csv
import datetime
import sys
from pathlib import Path
from statistics import mean, median


# ---------------------------------------------------------------------------
# Format detection + parsing
# ---------------------------------------------------------------------------

def parse_csv_header_and_metadata(path):
    """Return (header_columns, metadata_dict) where metadata_dict captures
    `# key=value` comment lines like reference_temp_c, label, output_type."""
    metadata = {}
    header = None
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("#"):
                # Strip leading "# "
                kv = line.lstrip("# ").strip()
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    metadata[k.strip()] = v.strip()
                continue
            # First non-comment non-blank line is the column header
            if line.strip():
                header = [c.strip() for c in line.split(",")]
                break
    return header, metadata


def detect_format(header):
    """Return one of 'brightness' / 'thermal' / None."""
    if not header:
        return None
    if header[0] == "brightness_pct" and "nits" in header:
        return "brightness"
    if header[0] == "backlight_temp_c" and "factor" in header:
        return "thermal"
    return None


def parse_brightness_lut(path):
    """Returns dict {brightness_pct -> nits}, OK rows only."""
    rows = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("brightness_pct"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 3:
                continue
            try:
                pct = int(parts[0])
                nits = float(parts[1])
                status = parts[2]
            except ValueError:
                continue
            if status != "OK":
                continue
            rows[pct] = nits
    return rows


def parse_thermal_factor(path):
    """Returns sorted list [(temp_c, factor)] from a thermal factor CSV."""
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("backlight_temp_c"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0])
                fac = float(parts[1])
            except ValueError:
                continue
            if fac <= 0:
                continue
            rows.append((t, fac))
    rows.sort(key=lambda r: r[0])
    return rows


def interpolate_at(rows, target):
    """Linear interpolation of rows[i][1] at rows[i][0] = target. Clamps at
    the endpoints. Caller guarantees rows is sorted ascending and non-empty."""
    if target <= rows[0][0]:
        return rows[0][1]
    if target >= rows[-1][0]:
        return rows[-1][1]
    for i in range(len(rows) - 1):
        t0, v0 = rows[i]
        t1, v1 = rows[i + 1]
        if t0 <= target <= t1:
            if t1 == t0:
                return (v0 + v1) / 2.0
            return v0 + (target - t0) / (t1 - t0) * (v1 - v0)
    return rows[-1][1]


# ---------------------------------------------------------------------------
# Blending
# ---------------------------------------------------------------------------

def blend_brightness(inputs):
    """inputs: list of dict {pct -> nits}. Returns sorted list [(pct, mean_nits)]."""
    # Use the intersection of pct values present in all inputs - we don't want
    # to average a row that's only in some inputs.
    common = None
    for d in inputs:
        common = set(d.keys()) if common is None else (common & d.keys())
    if not common:
        return []
    return sorted(
        [(p, mean(d[p] for d in inputs)) for p in common],
        key=lambda r: -r[0]  # descending pct, matches sweep convention
    )


def blend_thermal(inputs, target_ref_temp, bin_size=0.5):
    """inputs: list of [(temp, factor)]. Returns blended list [(temp, factor)]
    with factor(target_ref_temp) == 1.0 by construction. Each input is first
    renormalized to target_ref_temp (so they share the same baseline), then
    a union of bin-aligned temperature points is built. At each temp, only
    inputs whose measured range covers that temp contribute - inputs are
    interpolated within their range, never extrapolated."""
    # Renormalize each input
    renormed = []
    for rows in inputs:
        anchor = interpolate_at(rows, target_ref_temp)
        if anchor <= 0:
            continue
        renormed.append([(t, f / anchor) for t, f in rows])

    if not renormed:
        return []

    # Union of all temperature ranges, snapped to bin_size grid
    overall_min = min(rows[0][0] for rows in renormed)
    overall_max = max(rows[-1][0] for rows in renormed)
    overall_min = round(overall_min / bin_size) * bin_size
    overall_max = round(overall_max / bin_size) * bin_size

    grid = []
    t = overall_min
    while t <= overall_max + 1e-9:
        grid.append(round(t, 4))
        t += bin_size

    blended = []
    for t in grid:
        contributing = []
        for rows in renormed:
            # Only include this input if t is within its measured range
            if rows[0][0] <= t <= rows[-1][0]:
                contributing.append(interpolate_at(rows, t))
        if contributing:
            blended.append((t, mean(contributing)))
    return blended


# ---------------------------------------------------------------------------
# Output writers
# ---------------------------------------------------------------------------

def write_brightness_csv(out_path, rows, args, source_paths):
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as f:
        f.write("# als-dimmer brightness-to-nits sweep (BLENDED neutral)\n")
        if args.label:
            f.write(f"# label={args.label}\n")
        f.write(f"# blended_from={','.join(Path(s).name for s in source_paths)}\n")
        f.write(f"# blend_method=arithmetic_mean per brightness_pct\n")
        f.write(f"# generated_at={datetime.datetime.now().astimezone().isoformat()}\n")
        if rows:
            f.write(f"# nits_min={min(r[1] for r in rows):.3f} "
                    f"nits_max={max(r[1] for r in rows):.3f} rows={len(rows)}\n")
        f.write("brightness_pct,nits,status,backlight_temp_c\n")
        for pct, nits in rows:
            f.write(f"{pct},{nits:.4f},OK,\n")


def write_thermal_factor_csv(out_path, rows, args, source_paths, target_ref_temp):
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as f:
        f.write("# als-dimmer thermal compensation factor table (BLENDED neutral)\n")
        if args.label:
            f.write(f"# label={args.label}\n")
        f.write(f"# reference_temp_c={target_ref_temp:.2f}\n")
        f.write(f"# blended_from={','.join(Path(s).name for s in source_paths)}\n")
        f.write(f"# blend_method=arithmetic_mean per temp bin "
                f"(only inputs that cover each temp contribute)\n")
        f.write(f"# generated_at={datetime.datetime.now().astimezone().isoformat()}\n")
        if rows:
            f.write(f"# factor_min={min(r[1] for r in rows):.6f} "
                    f"factor_max={max(r[1] for r in rows):.6f} rows={len(rows)}\n")
        f.write("backlight_temp_c,factor\n")
        for t, fac in rows:
            f.write(f"{t:.2f},{fac:.6f}\n")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Blend two or more als-dimmer calibration CSVs into a "
                    "single neutral table.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:", 1)[-1] if "Examples:" in __doc__ else "",
    )
    ap.add_argument("--input", required=True, action="append", type=Path,
                    help="Input CSV (use --input multiple times). All inputs "
                         "must be the same format (brightness LUT or thermal "
                         "factor); the script auto-detects and refuses to mix.")
    ap.add_argument("--output", required=True, type=Path,
                    help="Path to write the blended CSV.")
    ap.add_argument("--label", default="neutral",
                    help="Free-form label written into the CSV header (default: neutral)")
    ap.add_argument("--target-reference-temp", type=float, default=None,
                    help="Thermal-factor blending only: reference temperature "
                         "to renormalize all inputs to before averaging "
                         "(default: median of inputs' reference_temp_c). Only "
                         "matters for thermal factor mode.")
    args = ap.parse_args()

    if len(args.input) < 2:
        print("error: at least two --input files required to blend", file=sys.stderr)
        return 1

    # Detect format from first input; verify rest match
    formats = []
    for p in args.input:
        if not p.exists():
            print(f"error: input not found: {p}", file=sys.stderr)
            return 1
        header, _ = parse_csv_header_and_metadata(p)
        fmt = detect_format(header)
        if fmt is None:
            print(f"error: cannot detect format of {p}; "
                  f"expected brightness_pct,nits,... or backlight_temp_c,factor,...",
                  file=sys.stderr)
            return 1
        formats.append(fmt)
    if len(set(formats)) > 1:
        print(f"error: inputs have mixed formats: {formats}", file=sys.stderr)
        return 1
    fmt = formats[0]
    print(f"info: detected {fmt} format across {len(args.input)} inputs",
          file=sys.stderr)

    if fmt == "brightness":
        inputs = [parse_brightness_lut(p) for p in args.input]
        rows = blend_brightness(inputs)
        if not rows:
            print("error: no brightness pcts in common across all inputs",
                  file=sys.stderr)
            return 1
        write_brightness_csv(args.output, rows, args, args.input)
        nmin = min(r[1] for r in rows)
        nmax = max(r[1] for r in rows)
        print(f"wrote {len(rows)} blended brightness rows to {args.output}")
        print(f"  nits range = {nmin:.2f} .. {nmax:.2f}")
    else:
        # Pick target reference temp: explicit > median of inputs' refs
        if args.target_reference_temp is not None:
            target_ref = args.target_reference_temp
        else:
            ref_temps = []
            for p in args.input:
                _, md = parse_csv_header_and_metadata(p)
                if "reference_temp_c" in md:
                    try:
                        ref_temps.append(float(md["reference_temp_c"]))
                    except ValueError:
                        pass
            if ref_temps:
                target_ref = median(ref_temps)
                print(f"info: --target-reference-temp not specified, using "
                      f"median of inputs' reference temps: {target_ref:.2f} C",
                      file=sys.stderr)
            else:
                target_ref = 38.5
                print(f"info: no reference_temp_c found in inputs, defaulting to "
                      f"{target_ref} C", file=sys.stderr)

        inputs = [parse_thermal_factor(p) for p in args.input]
        if any(not rows for rows in inputs):
            print("error: at least one input has no usable thermal factor rows",
                  file=sys.stderr)
            return 1
        rows = blend_thermal(inputs, target_ref)
        if not rows:
            print("error: no overlapping temperature range across all inputs",
                  file=sys.stderr)
            return 1
        write_thermal_factor_csv(args.output, rows, args, args.input, target_ref)
        fmin = min(r[1] for r in rows)
        fmax = max(r[1] for r in rows)
        print(f"wrote {len(rows)} blended thermal factor rows to {args.output}")
        print(f"  reference_temp_c = {target_ref:.2f}")
        print(f"  factor range     = {fmin:.4f} .. {fmax:.4f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
