#!/bin/sh
# ==============================================================================
# ALS-Dimmer I2C Bus Stress Test
# ==============================================================================
# Ramps brightness up and down in a loop to stress-test I2C bus contention
# between als-dimmer and other I2C devices (e.g., himax touch controller).
#
# Usage:
#   ./als-dimmer-auto-test.sh --brlow=5 --brhigh=99 --waitms=200 --loopcount=20
# ==============================================================================

set -e

# Defaults
BR_LOW=5
BR_HIGH=99
WAIT_MS=200
LOOP_COUNT=20
CLIENT="als-dimmer-client"

# Parse arguments
for arg in "$@"; do
    case "$arg" in
        --brlow=*)    BR_LOW="${arg#*=}" ;;
        --brhigh=*)   BR_HIGH="${arg#*=}" ;;
        --waitms=*)   WAIT_MS="${arg#*=}" ;;
        --loopcount=*) LOOP_COUNT="${arg#*=}" ;;
        --client=*)   CLIENT="${arg#*=}" ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --brlow=N       Lower brightness bound (default: 5)"
            echo "  --brhigh=N      Upper brightness bound (default: 99)"
            echo "  --waitms=N      Wait time in ms between steps (default: 200)"
            echo "  --loopcount=N   Number of ramp-up/down cycles (default: 20)"
            echo "  --client=PATH   Path to als-dimmer-client (default: als-dimmer-client)"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

# Validate
if [ "$BR_LOW" -ge "$BR_HIGH" ]; then
    echo "[ERROR] --brlow ($BR_LOW) must be less than --brhigh ($BR_HIGH)"
    exit 1
fi

if ! command -v "$CLIENT" >/dev/null 2>&1; then
    echo "[ERROR] $CLIENT not found in PATH"
    exit 1
fi

WAIT_SEC=$(awk "BEGIN {printf \"%.3f\", $WAIT_MS / 1000.0}")

echo "======================================"
echo "  ALS-Dimmer I2C Bus Stress Test"
echo "======================================"
echo "  Brightness range: $BR_LOW - $BR_HIGH"
echo "  Wait between steps: ${WAIT_MS}ms"
echo "  Ramp-up/down cycles: $LOOP_COUNT"
echo "  Client: $CLIENT"
echo "======================================"
echo ""

for cycle in $(seq 1 "$LOOP_COUNT"); do
    echo "[Cycle $cycle/$LOOP_COUNT] Ramp UP: $BR_LOW -> $BR_HIGH"
    for br in $(seq "$BR_LOW" "$BR_HIGH"); do
        $CLIENT --brightness="$br" >/dev/null 2>&1 || echo "  [WARN] Failed to set brightness=$br"
        sleep "$WAIT_SEC"
    done

    echo "[Cycle $cycle/$LOOP_COUNT] Ramp DOWN: $BR_HIGH -> $BR_LOW"
    for br in $(seq "$BR_HIGH" -1 "$BR_LOW"); do
        $CLIENT --brightness="$br" >/dev/null 2>&1 || echo "  [WARN] Failed to set brightness=$br"
        sleep "$WAIT_SEC"
    done

    echo "[Cycle $cycle/$LOOP_COUNT] Complete"
    echo ""
done

echo "======================================"
echo "  Stress test complete: $LOOP_COUNT cycles"
echo "======================================"
