#!/usr/bin/env python3
"""
als-dimmer-sweep.py - drive the daemon through brightness 100..0% and record
                      the displayed luminance at each step into a CSV.

The CSV produced here is fed back to the daemon via:
    "brightness_to_nits": {"enabled": true, "sweep_table": "..."}
Once loaded, get_absolute_brightness / set_absolute_brightness become useful.

Why not poke the output device directly? The daemon already abstracts away
boe_pwm / dimmer800 / dimmer2048 / fpga_sysfs / ddcutil etc. By driving
through the daemon's set_brightness command, the same sweep script works
for every output type.

Workflow:
  1. Place a full-white patch on the area the colorimeter is measuring (use
     --pre-cmd to switch the display to full-white automatically; otherwise
     prepare it manually before running).
  2. Make sure spotread (Argyll-CMS) finds the colorimeter and is calibrated.
  3. Make sure als-dimmer is running with a working output.
  4. Run this script. Saved current mode/brightness are restored at exit.

Examples:
  # Default: TCP localhost:9000, descending 100..0, 3s settle, no temp.
  ./als-dimmer-sweep.py --output sweep_warm.csv --label warm

  # Unix socket, custom step size, with F1KM backlight NTC temp.
  ./als-dimmer-sweep.py \\
      --socket /tmp/als-dimmer.sock \\
      --step 5 --output sweep_warm_5pct.csv --label warm \\
      --temp-cmd 'i2ctransfer -y 1 w2@0x66 0x10 0x02 r2@0x66 | python3 -c "import sys,struct; r=sys.stdin.read().split(); print(struct.unpack(\\">h\\", bytes(int(x,16) for x in r))[0]/10.0)"'

  # sysfs hwmon temperature source (NTC exposed via lm-sensors / a kernel driver).
  ./als-dimmer-sweep.py --output sweep_hot.csv --label hot \\
      --temp-cmd 'awk "{print \\$1/1000.0}" /sys/class/hwmon/hwmon3/temp1_input'

  # BOE Pi deployment: pre-cmd/post-cmd auto-default to micropanel's
  # launcher-client when it's present at ~/micropanel/usr/bin/launcher-client,
  # so the typical invocation is just:
  ./als-dimmer-sweep.py --output sweep_warm.csv --label warm --warmup-seconds 5 \\
      --temp-cmd 'disptool --device=ioc --command=bltemp --autotestformat | sed -E "s/^.*Temperature[^:]*:\\s*([0-9.-]+).*/\\1/"'

  # Override the auto-default with a custom pattern source:
  ./als-dimmer-sweep.py --output sweep_warm.csv --label warm \\
      --pre-cmd  'my-pattern-tool set white' \\
      --post-cmd 'my-pattern-tool set black'

  # Explicitly disable the auto-default (e.g., screen is already full-white):
  ./als-dimmer-sweep.py --output sweep_warm.csv --label warm --pre-cmd '' --post-cmd ''

CSV format (matches what the daemon's BrightnessToNitsLut expects):

  # label=warm
  # output_type=boe_pwm
  # timestamp=2026-04-28T12:34:56+00:00
  # temp_source_cmd=...
  brightness_pct,nits,status,backlight_temp_c
  100,1119.80,OK,42.5
  99,1110.40,OK,42.7
  ...
  0,0.05,OK,
"""

import argparse
import csv
import datetime
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time


# --- daemon JSON-line protocol -----------------------------------------------

class DaemonClient:
    def __init__(self, socket_path=None, host="127.0.0.1", port=9000, timeout=5.0):
        if socket_path:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(timeout)
            self._sock.connect(socket_path)
        else:
            self._sock = socket.create_connection((host, port), timeout=timeout)

    def call(self, command, **params):
        msg = {"command": command}
        if params:
            msg["params"] = params
        self._sock.sendall((json.dumps(msg) + "\n").encode())
        # Read until we get one full JSON object back. Daemon sends a single
        # response per command; usually fits in one recv but loop just in case.
        buf = b""
        while True:
            chunk = self._sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            try:
                return json.loads(buf.decode())
            except json.JSONDecodeError:
                continue
        raise RuntimeError("daemon closed connection without sending a response")

    def close(self):
        try:
            self._sock.close()
        except OSError:
            pass


# --- spotread driver ---------------------------------------------------------

_YXY_RE = re.compile(r"Result is XYZ:.*Yxy:\s*([\d.]+)")


def parse_y_from_spotread(text):
    """Pull the Y (nits) value from spotread's "Result is XYZ: ..., Yxy: Y x y" line."""
    m = _YXY_RE.search(text)
    return float(m.group(1)) if m else None


def measure_nits(max_retries, retry_sleep_s):
    """Run spotread once per attempt; return (nits_or_None, retries_used)."""
    retries = 0
    for attempt in range(max_retries + 1):
        try:
            out = subprocess.check_output(
                ["spotread", "-x", "-O"],
                stderr=subprocess.STDOUT,
                timeout=15,
            ).decode(errors="replace")
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired,
                FileNotFoundError) as e:
            out = str(e)

        nits = parse_y_from_spotread(out)
        if nits is not None:
            return nits, retries
        retries += 1
        if attempt < max_retries:
            print(f"    retry {retries}/{max_retries} (spotread parse failed)",
                  file=sys.stderr)
            time.sleep(retry_sleep_s)
    return None, retries


# --- platform default pre/post commands -------------------------------------
# The default --pre-cmd and --post-cmd wire the sweep into the micropanel
# launcher-client display-pattern controller used on the BOE Pi deployment.
# These defaults silently skip when launcher-client isn't installed at the
# expected path, so the script still works on systems without micropanel
# (the user is responsible for arranging a full-white screen patch some
# other way, or passing --pre-cmd explicitly).
#
# To explicitly disable the defaults even when launcher-client IS installed,
# pass --pre-cmd '' (empty string).

# Live progress overlay shown via disp-tester's `text` command (bottom-right
# corner, semi-transparent black box, 16px white text). \\n in the template
# renders as a newline on disp-tester's overlay. Available format placeholders:
#   {step} {total} {brightness_pct}
_DEFAULT_PROGRESS_TEMPLATE = (
    "Brightness Calibration in Progress\\n"
    "Step: {step}/{total}\\n"
    "Brightness: {brightness_pct}%"
)
_INITIAL_OVERLAY_TEXT = "Starting calibration sweep..."

_DEFAULT_LAUNCHER_CLIENT = os.path.expanduser("~/micropanel/usr/bin/launcher-client")

# Two-step pre-cmd: ask qt-demo-launcher (port 8081) to start the
# "pattern-generator" app (which spawns disp-tester listening on 8082),
# wait briefly for it to come up, then ask disp-tester for full-white.
# Chained with `;` rather than `&&` so an "app-already-running" response
# from start-app doesn't prevent the pattern white call - the pattern
# white step is the real success check, since pre-cmd's exit code comes
# from the last command in the chain.
_DEFAULT_PRE_CMD = (
    f'{_DEFAULT_LAUNCHER_CLIENT} '
    '--srv=127.0.0.1:8081 --command="start-app pattern-generator" --timeoutsec=2 ; '
    'sleep 1 ; '
    f'{_DEFAULT_LAUNCHER_CLIENT} '
    '--srv=127.0.0.1:8082 --command="pattern white" --timeoutsec=2'
)
# Cleanup: pattern black on disp-tester (so the brief moment before stop-app
# tears it down is dark, not white), then stop-app so qt-demo-launcher's home
# screen comes back where the user started. (set-metadata-text was tried
# initially but disp-tester reliably drops the connection on an empty arg
# - and stop-app destroys all metadata state anyway, so it's redundant.)
_DEFAULT_POST_CMD = (
    f'{_DEFAULT_LAUNCHER_CLIENT} '
    '--srv=127.0.0.1:8082 --command="pattern black" --timeoutsec=2 ; '
    f'{_DEFAULT_LAUNCHER_CLIENT} '
    '--srv=127.0.0.1:8081 --command="stop-app" --timeoutsec=2'
)


def validate_output_path(path):
    """Verify the output CSV path is writable BEFORE the sweep starts so the
    user doesn't spend 5+ minutes of measurement only to discover at the end
    that the destination directory needs sudo. We open in append mode so we
    don't truncate any existing file at this point."""
    try:
        with open(path, "a"):
            pass
    except OSError as e:
        print(f"error: cannot write --output {path}: {e}", file=sys.stderr)
        print(f"hint: pick a path under your home or /tmp, e.g. "
              f"--output ~/sweeps/your-name.csv", file=sys.stderr)
        return False
    return True


def apply_platform_defaults(args):
    """If --pre-cmd / --post-cmd / --progress-text weren't supplied (None),
    fill in the platform default when the referenced launcher-client binary
    is executable. Otherwise leave as empty (skip). Explicit empty strings
    from the command line are preserved so the user can opt out."""
    launcher_present = os.access(_DEFAULT_LAUNCHER_CLIENT, os.X_OK)
    for attr, default in (("pre_cmd", _DEFAULT_PRE_CMD),
                          ("post_cmd", _DEFAULT_POST_CMD),
                          ("progress_text", _DEFAULT_PROGRESS_TEMPLATE)):
        if getattr(args, attr) is None:
            if launcher_present:
                setattr(args, attr, default)
                print(f"using default --{attr.replace('_', '-')} "
                      f"(launcher-client detected)", file=sys.stderr)
            else:
                setattr(args, attr, "")


def _shell_escape_sq(s):
    """Escape any single quotes in s so it's safe inside a single-quoted
    shell argument (the standard '\\'' trick)."""
    return s.replace("'", "'\\''")


def set_overlay_text(message):
    """Push `message` to disp-tester's bottom-right metadata overlay via
    launcher-client. Uses disp-tester's `set-metadata-text` command (which
    takes everything after the first space as the message and converts
    literal `\\n` into real newlines internally - see PatternController.cpp).
    Fire-and-forget with a short timeout so a slow or sick disp-tester
    can't drag the sweep out of timing. Caller is responsible for gating
    (typically `if args.progress_text:`) and for having previously called
    enable_overlay_visibility() at least once - by default the overlay
    starts in `autohide` mode."""
    if not message:
        return
    cmd = (f'{_DEFAULT_LAUNCHER_CLIENT} --srv=127.0.0.1:8082 '
           f'--command=\'set-metadata-text {_shell_escape_sq(message)}\' '
           f'--timeoutsec=1')
    try:
        subprocess.run(["sh", "-c", cmd],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL,
                       timeout=2)
    except (subprocess.TimeoutExpired, OSError):
        pass  # overlay update is best-effort - never block the sweep


def enable_overlay_visibility():
    """disp-tester's metadata visibility defaults to "autohide" - explicitly
    set it to "enable" so per-step set-metadata-text calls stay visible
    throughout the sweep. State is sticky inside disp-tester until the
    next set-metadata-status (or disp-tester restart). Call this once
    after disp-tester is up but before the first set_overlay_text."""
    cmd = (f'{_DEFAULT_LAUNCHER_CLIENT} --srv=127.0.0.1:8082 '
           f'--command="set-metadata-status enable" --timeoutsec=1')
    try:
        subprocess.run(["sh", "-c", cmd],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL,
                       timeout=2)
    except (subprocess.TimeoutExpired, OSError):
        pass


def run_pre_cmd(cmd):
    """Run --pre-cmd. Returns True on success. Hard-fail (caller aborts) on
    any non-zero exit, since measuring against wrong content produces a
    meaningless CSV."""
    if not cmd:
        return True
    print(f"running pre-cmd: {cmd}", file=sys.stderr)
    try:
        subprocess.run(["sh", "-c", cmd], check=True, timeout=30)
        return True
    except subprocess.CalledProcessError as e:
        print(f"error: --pre-cmd exited rc={e.returncode}; aborting sweep", file=sys.stderr)
        return False
    except subprocess.TimeoutExpired:
        print("error: --pre-cmd timed out (30s); aborting sweep", file=sys.stderr)
        return False
    except FileNotFoundError as e:
        print(f"error: --pre-cmd: {e}; aborting sweep", file=sys.stderr)
        return False


def run_post_cmd(cmd):
    """Run --post-cmd. Failures are warned-but-not-fatal: the CSV is already
    written by the time post-cmd runs, and forcing an error here would obscure
    that data was successfully captured."""
    if not cmd:
        return
    print(f"running post-cmd: {cmd}", file=sys.stderr)
    try:
        subprocess.run(["sh", "-c", cmd], timeout=30)
    except subprocess.TimeoutExpired:
        print("warning: --post-cmd timed out (30s)", file=sys.stderr)
    except FileNotFoundError as e:
        print(f"warning: --post-cmd: {e}", file=sys.stderr)


def read_temp(temp_cmd):
    """Run --temp-cmd, parse first float from stdout. Returns None on any error."""
    if not temp_cmd:
        return None
    try:
        out = subprocess.check_output(
            ["sh", "-c", temp_cmd], stderr=subprocess.DEVNULL, timeout=5
        ).decode(errors="replace").strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"-?\d+(\.\d+)?", out)
    return float(m.group(0)) if m else None


# --- main --------------------------------------------------------------------

def build_step_list(start, end, step):
    """Inclusive list from start to end with the given step (descending if start>end)."""
    if start == end:
        return [start]
    if start > end:
        steps = list(range(start, end - 1, -abs(step)))
        if steps[-1] != end:
            steps.append(end)
    else:
        steps = list(range(start, end + 1, abs(step)))
        if steps[-1] != end:
            steps.append(end)
    return steps


def main():
    ap = argparse.ArgumentParser(
        description="Run a brightness-to-nits sweep through als-dimmer and write a CSV.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:", 1)[1] if "Examples:" in __doc__ else "",
    )
    # Connection
    ap.add_argument("--socket", help="Unix socket path (overrides --host/--port)")
    ap.add_argument("--host", default="127.0.0.1", help="TCP host (default: 127.0.0.1)")
    ap.add_argument("--port", type=int, default=9000, help="TCP port (default: 9000)")

    # Sweep shape
    ap.add_argument("--start", type=int, default=100, help="Starting brightness %% (default: 100)")
    ap.add_argument("--end",   type=int, default=0,   help="Ending brightness %% (default: 0)")
    ap.add_argument("--step",  type=int, default=1,   help="Step size in %% (default: 1)")
    ap.add_argument("--ascending", action="store_true",
                    help="Sweep 0..100 instead of the default 100..0")

    # Timing & retries
    ap.add_argument("--settle-seconds", type=float, default=3.0,
                    help="Seconds to wait after each set_brightness before measuring (default: 3.0)")
    ap.add_argument("--max-retries", type=int, default=5,
                    help="spotread parse retries per row (default: 5)")
    ap.add_argument("--retry-sleep", type=float, default=1.0,
                    help="Seconds between spotread retries (default: 1.0)")
    ap.add_argument("--max-consecutive-failures", type=int, default=10,
                    help="Abort if this many rows in a row fail (default: 10)")

    # Side data
    ap.add_argument("--temp-cmd",
                    help="Shell command that prints backlight temperature in degC. "
                         "Stdout is parsed for the first float. Empty/failed -> column blank.")
    ap.add_argument("--label", default="",
                    help="Free-form label written into CSV header (e.g. cold/warm/hot)")

    # Display content / warmup hooks. None = use platform default (micropanel/
    # launcher-client) if available, else skip. Empty string = explicitly skip.
    ap.add_argument("--pre-cmd", default=None,
                    help="Shell command run after MANUAL is forced and before the sweep "
                         "starts (e.g. switch the display to full-white via disp-tester). "
                         "Non-zero exit aborts the sweep - measuring against the wrong "
                         "screen content would produce a meaningless CSV. "
                         "If omitted, defaults to a two-step micropanel sequence: "
                         "start-app pattern-generator on qt-demo-launcher (8081), "
                         "wait 1s for disp-tester to come up, then pattern white on "
                         "disp-tester (8082). Default is used only when "
                         "~/micropanel/usr/bin/launcher-client is installed; otherwise "
                         "skipped. Pass --pre-cmd '' to disable.")
    ap.add_argument("--post-cmd", default=None,
                    help="Shell command run after the sweep finishes OR on Ctrl-C/SIGTERM "
                         "(e.g. restore the original screen content). Failure here is a "
                         "warning, not fatal - the CSV is already written. If omitted, "
                         "defaults to: pattern black + clear metadata on disp-tester, "
                         "then stop-app on qt-demo-launcher (returns to home screen). "
                         "Same detection-and-skip semantics as --pre-cmd. "
                         "Pass --post-cmd '' to disable.")
    ap.add_argument("--warmup-seconds", type=float, default=0.0,
                    help="After --pre-cmd, set the start brightness explicitly and sleep "
                         "this many seconds before measuring step 1. Useful when the panel "
                         "or colorimeter benefits from extra time to settle on the first "
                         "row (default: 0 = skip)")
    ap.add_argument("--progress-text", default=None,
                    help="Format template for the bottom-right progress overlay shown via "
                         "disp-tester's `text` command. Available placeholders: "
                         "{step} {total} {brightness_pct}. Use \\n for line breaks. "
                         "If omitted, defaults to a 3-line 'Brightness Calibration in "
                         "Progress / Step / Brightness' template when launcher-client is "
                         "installed; otherwise the overlay is skipped. "
                         "Pass --progress-text '' to disable.")

    # Output
    ap.add_argument("--output", required=True, help="Path to write the CSV")

    # Behavior toggles
    ap.add_argument("--no-restore", action="store_true",
                    help="Don't restore the original mode/brightness on exit (debug only)")

    args = ap.parse_args()
    apply_platform_defaults(args)

    # Catch unwritable --output paths up-front so the user doesn't waste a
    # 5-minute sweep on a destination that turns out to need sudo.
    if not validate_output_path(args.output):
        return 1

    # Validate the progress-text template once now (rather than once per step).
    if args.progress_text:
        try:
            args.progress_text.format(step=1, total=100, brightness_pct=50)
        except (KeyError, IndexError) as e:
            print(f"warning: --progress-text references unknown placeholder {e}; "
                  f"available: {{step}} {{total}} {{brightness_pct}}; "
                  f"disabling overlay", file=sys.stderr)
            args.progress_text = ""

    # Validate sweep bounds
    if not (0 <= args.start <= 100) or not (0 <= args.end <= 100):
        print("error: --start and --end must be in [0, 100]", file=sys.stderr)
        return 2
    if args.step <= 0:
        print("error: --step must be > 0", file=sys.stderr)
        return 2

    # Apply --ascending flag
    if args.ascending:
        steps = build_step_list(min(args.start, args.end), max(args.start, args.end), args.step)
    else:
        # Default: high to low
        s_hi, s_lo = max(args.start, args.end), min(args.start, args.end)
        steps = build_step_list(s_hi, s_lo, args.step)

    # Connect to daemon, capture original state, force MANUAL
    print(f"connecting to daemon ({'socket=' + args.socket if args.socket else f'tcp={args.host}:{args.port}'})...",
          file=sys.stderr)
    try:
        client = DaemonClient(args.socket, args.host, args.port)
    except (OSError, socket.timeout) as e:
        print(f"error: cannot connect to daemon: {e}", file=sys.stderr)
        return 1

    try:
        st = client.call("get_status")
        if st.get("status") != "success":
            print(f"error: get_status failed: {st}", file=sys.stderr)
            return 1
        original_mode = st["data"]["mode"]
        original_brightness = st["data"]["brightness"]
        sensor_status = st["data"].get("sensor_status", "available")

        cfg = client.call("get_config")
        output_type = cfg["data"].get("output_type", "unknown") \
            if cfg.get("status") == "success" else "unknown"
    except (KeyError, json.JSONDecodeError) as e:
        print(f"error: unexpected daemon response: {e}", file=sys.stderr)
        return 1

    print(f"daemon: output_type={output_type} sensor={sensor_status} "
          f"original mode={original_mode} brightness={original_brightness}%",
          file=sys.stderr)

    # Force MANUAL so the daemon doesn't override our brightness writes from AUTO.
    # If sensor is unavailable the daemon is already in MANUAL.
    if original_mode != "manual":
        r = client.call("set_mode", mode="manual")
        if r.get("status") != "success":
            print(f"error: set_mode manual failed: {r.get('message')}", file=sys.stderr)
            client.close()
            return 1

    # Restore on Ctrl-C / kill / normal exit. We always at least try to write
    # whatever rows we collected, so the sweep is never wasted.
    rows_collected = []
    aborted_reason = None

    def _restore_and_write():
        if not args.no_restore:
            try:
                # Restore brightness while still in MANUAL so AUTO mode doesn't
                # flip to MANUAL_TEMPORARY on the set_brightness call.
                if original_mode == "manual":
                    client.call("set_brightness", brightness=original_brightness)
                else:
                    # AUTO (or MANUAL_TEMPORARY) doesn't persist - flip back to
                    # AUTO and let its zone mapping recompute brightness.
                    client.call("set_mode", mode="auto")
            except (OSError, socket.timeout):
                pass
        try:
            client.close()
        except OSError:
            pass
        # Restore screen content (also runs on Ctrl-C / SIGTERM via _signal_handler).
        run_post_cmd(args.post_cmd)
        if rows_collected or aborted_reason:
            _safe_write_csv(args, output_type, steps, rows_collected, aborted_reason)

    def _signal_handler(signum, _frame):
        nonlocal aborted_reason
        aborted_reason = f"signal {signum}"
        print(f"\ncaught signal {signum}, restoring and writing partial CSV...",
              file=sys.stderr)
        _restore_and_write()
        sys.exit(130 if signum == signal.SIGINT else 143)

    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    # ---- pre-sweep hooks ---------------------------------------------------
    # Run --pre-cmd to set the display content (e.g., switch to full-white).
    # The signal handlers above are already installed, so a Ctrl-C during a
    # long-running pre-cmd still triggers _restore_and_write -> post-cmd.
    if not run_pre_cmd(args.pre_cmd):
        _restore_and_write()
        return 2

    # Show an initial overlay message while warmup + first measurement are
    # in flight, so the operator sees something between pre-cmd completion
    # and the first per-step update. Disp-tester defaults to `autohide`,
    # so we explicitly enable visibility once before the first text call.
    if args.progress_text:
        enable_overlay_visibility()
        set_overlay_text(_INITIAL_OVERLAY_TEXT)

    # Optional warmup: jump to the start brightness up-front and sleep, so
    # the panel and colorimeter have time to settle before step 1's reading
    # (in addition to the per-step --settle-seconds).
    if args.warmup_seconds > 0 and steps:
        print(f"warmup: setting brightness {steps[0]}% and sleeping "
              f"{args.warmup_seconds}s before measuring", file=sys.stderr)
        try:
            client.call("set_brightness", brightness=int(steps[0]))
        except (OSError, socket.timeout) as e:
            print(f"error: warmup set_brightness failed: {e}", file=sys.stderr)
            _restore_and_write()
            return 1
        time.sleep(args.warmup_seconds)

    # ---- run the sweep -----------------------------------------------------
    print(f"sweeping {len(steps)} steps -> {args.output}", file=sys.stderr)
    print("--------", file=sys.stderr)

    consecutive_failures = 0
    for idx, pct in enumerate(steps, start=1):
        r = client.call("set_brightness", brightness=int(pct))
        if r.get("status") != "success":
            print(f"  [{idx}/{len(steps)}] set_brightness {pct}%% failed: {r.get('message')}",
                  file=sys.stderr)
            rows_collected.append((pct, None, "FAIL", None, 0))
            consecutive_failures += 1
            if consecutive_failures >= args.max_consecutive_failures:
                aborted_reason = f"{consecutive_failures} consecutive failures"
                print(f"abort: {aborted_reason}", file=sys.stderr)
                break
            continue

        # Update the bottom-right overlay before settling so it's already in
        # place when the colorimeter reads. Best-effort - failures don't
        # interrupt the sweep.
        if args.progress_text:
            try:
                msg = args.progress_text.format(step=idx, total=len(steps),
                                                brightness_pct=int(pct))
                set_overlay_text(msg)
            except (KeyError, IndexError):
                pass  # already validated at startup, but defensive

        time.sleep(args.settle_seconds)
        nits, retries = measure_nits(args.max_retries, args.retry_sleep)
        temp = read_temp(args.temp_cmd)
        status = "OK" if nits is not None else "FAIL"

        if nits is None:
            consecutive_failures += 1
        else:
            consecutive_failures = 0

        rows_collected.append((pct, nits, status, temp, retries))
        nits_disp = f"{nits:>9.4f}" if nits is not None else "       NA"
        temp_disp = f" temp={temp:.1f}C" if temp is not None else ""
        print(f"  [{idx:>3}/{len(steps)}] {pct:>3}% -> nits={nits_disp} "
              f"({status}, retries={retries}){temp_disp}",
              file=sys.stderr)

        if consecutive_failures >= args.max_consecutive_failures:
            aborted_reason = f"{consecutive_failures} consecutive measurement failures"
            print(f"abort: {aborted_reason}", file=sys.stderr)
            break

    # Normal completion
    _restore_and_write()
    print("--------", file=sys.stderr)
    print(f"sweep complete -> {args.output}", file=sys.stderr)
    return 0


def _safe_write_csv(args, output_type, planned_steps, rows, aborted_reason):
    """Write the CSV at args.output. If that fails (permission denied, disk
    full, path removed mid-run, etc.) fall back to a timestamped path under
    /tmp so the data isn't silently lost. The pre-flight validate_output_path
    check should normally catch the common case, but this is a safety net."""
    try:
        _write_csv(args, output_type, planned_steps, rows, aborted_reason)
        return
    except OSError as e:
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        fallback = f"/tmp/als-dimmer-sweep-{ts}.csv"
        print(f"warning: writing {args.output} failed ({e}); "
              f"saving to {fallback} instead", file=sys.stderr)
        try:
            _write_csv(args, output_type, planned_steps, rows, aborted_reason,
                       out_path=fallback)
            print(f"data saved to {fallback}", file=sys.stderr)
        except OSError as e2:
            print(f"error: fallback write also failed: {e2}", file=sys.stderr)
            print(f"data lost - {len(rows)} rows could not be saved",
                  file=sys.stderr)


def _write_csv(args, output_type, planned_steps, rows, aborted_reason,
               out_path=None):
    if out_path is None:
        out_path = args.output
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        f.write("# als-dimmer brightness-to-nits sweep\n")
        if args.label:
            f.write(f"# label={args.label}\n")
        f.write(f"# output_type={output_type}\n")
        f.write(f"# timestamp={datetime.datetime.now().astimezone().isoformat()}\n")
        f.write(f"# step_pct={args.step} settle_seconds={args.settle_seconds} "
                f"max_retries={args.max_retries}\n")
        f.write(f"# planned_steps={len(planned_steps)} actual_rows={len(rows)}\n")
        if args.temp_cmd:
            # Don't write multi-line / very long commands; truncate for readability.
            cmd_one = " ".join(args.temp_cmd.split())
            if len(cmd_one) > 200:
                cmd_one = cmd_one[:197] + "..."
            f.write(f"# temp_source_cmd={cmd_one}\n")
        if args.pre_cmd:
            cmd_one = " ".join(args.pre_cmd.split())
            if len(cmd_one) > 200:
                cmd_one = cmd_one[:197] + "..."
            f.write(f"# pre_cmd={cmd_one}\n")
        if args.warmup_seconds > 0:
            f.write(f"# warmup_seconds={args.warmup_seconds}\n")
        if aborted_reason:
            f.write(f"# aborted={aborted_reason}\n")

        # Linearity summary (skip if too few OK rows)
        ok_rows = [r for r in rows if r[2] == "OK" and r[1] is not None]
        if len(ok_rows) >= 2:
            ok_sorted = sorted(ok_rows, key=lambda r: r[0])
            min_n, max_n = ok_sorted[0][1], ok_sorted[-1][1]
            f.write(f"# nits_min={min_n:.3f} nits_max={max_n:.3f} ok_rows={len(ok_rows)}\n")

        w.writerow(["brightness_pct", "nits", "status", "backlight_temp_c"])
        for pct, nits, status, temp, _retries in rows:
            nits_str = f"{nits:.4f}" if nits is not None else ""
            temp_str = f"{temp:.2f}" if temp is not None else ""
            w.writerow([pct, nits_str, status, temp_str])


if __name__ == "__main__":
    sys.exit(main())
