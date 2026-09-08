#!/usr/bin/env bash
# Launch the port through the intro N times and tabulate how each run ended.
#
# This exists for docs/issues/003. That bug is intermittent -- three launches on
# the reporter's machine gave two crashes and one clean nine-minute session, on
# the same build with the same inputs -- and nothing about it can be worked on
# one run at a time. A fix is only a fix if a run of twenty says so, and a
# hypothesis is only eliminated the same way.
#
# It drives the HEADLESS build (RAYMAN2_ENABLE_FRONTEND=OFF), because that one
# starts the game itself from the ROM on its command line. The frontend build
# waits for somebody to click Start, which cannot be scripted and would measure
# the launcher rather than the game.
#
# Each run is killed after --seconds and that is not a failure: the intro is
# minutes long and the crash under investigation lands in the first few seconds
# of it. A run that is still going when the clock runs out is a run that did not
# crash, which is the whole measurement.
#
# Environment passed through: anything already exported. The switches worth
# turning are
#
#     RAYMAN2_NOAUDIOUCODE=1   run without the audio microcode
#     RAYMAN2_YIELD_MS=<n>     change how long a spinning game thread waits
#
# both of which are described in docs/issues/003.
#
# To soak the SHIPPED configuration -- the frontend build, which is what every
# report so far came from -- point --build at it and set RAYMAN2_AUTOSTART=1,
# which presses Start on the launcher:
#
#     RAYMAN2_AUTOSTART=1 scripts/soak.sh --build build-fe
#
# That needs a ROM already ingested into the config directory, because the
# launcher is what normally asks for one.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

RUNS=20
SECONDS_PER_RUN=25
BUILD="build-headless"
ROM="rom.z64"

usage() {
    cat >&2 <<USAGE
usage: scripts/soak.sh [--runs N] [--seconds N] [--build DIR] [--rom PATH]

    --runs      how many launches            (default $RUNS)
    --seconds   how long to let each one run (default $SECONDS_PER_RUN)
    --build     which build directory        (default $BUILD)
    --rom       the cartridge dump           (default $ROM)
USAGE
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --runs)    RUNS="$2";            shift 2 ;;
        --seconds) SECONDS_PER_RUN="$2"; shift 2 ;;
        --build)   BUILD="$2";           shift 2 ;;
        --rom)     ROM="$2";             shift 2 ;;
        -h|--help) usage ;;
        *)         echo "unknown argument: $1" >&2; usage ;;
    esac
done

EXE="$ROOT/$BUILD/rayman2-recomp.exe"
[ -x "$EXE" ] || EXE="$ROOT/$BUILD/rayman2-recomp"
if [ ! -x "$EXE" ]; then
    echo "no executable in $BUILD -- build it first" >&2
    exit 1
fi
if [ ! -f "$ROOT/$ROM" ]; then
    echo "$ROM not found" >&2
    exit 1
fi

REPORTS="$ROOT/$BUILD/debug-report"
OUT="$ROOT/$BUILD/soak-$(date +%Y-%m-%d_%H%M%S)"
mkdir -p "$OUT"

echo "soak: $RUNS runs of ${SECONDS_PER_RUN}s from $BUILD"
echo "      reports kept in $OUT"
echo "      RAYMAN2_NOAUDIOUCODE=${RAYMAN2_NOAUDIOUCODE:-unset}  RAYMAN2_YIELD_MS=${RAYMAN2_YIELD_MS:-unset}  RAYMAN2_AUTOSTART=${RAYMAN2_AUTOSTART:-unset}"
echo

survived=0
crashed=0
other=0

for i in $(seq 1 "$RUNS"); do
    rm -rf "$REPORTS"
    # SIGKILL rather than SIGTERM: the point is to end the run, not to test the
    # shutdown path, and a report with no SUMMARY is expected here for exactly
    # that reason. Only the CRASH blocks are being counted.
    # Through a nested shell whose stderr is discarded. A shell announces
    # "Killed" for every foreground job a signal ends, and it announces it on
    # its OWN stderr -- so the redirection has to be outside a shell that is not
    # this one, or twenty of those bury the table this script exists to print.
    bash -c 'timeout -s KILL "$1" "$2" "$3" >/dev/null 2>&1' _          "$SECONDS_PER_RUN" "$EXE" "$ROOT/$ROM" 2>/dev/null
    code=$?

    report="$(ls -1 "$REPORTS"/*.txt 2>/dev/null | head -1)"
    kind="(no report)"
    if [ -n "$report" ]; then
        kind="$(grep -m1 '^  kind: ' "$report" | sed 's/^  kind: //')"
        [ -n "$kind" ] || kind="-"
        cp "$report" "$OUT/run-$(printf '%02d' "$i").txt"
    fi

    # 137 is the SIGKILL from timeout: the run outlived the clock, which is the
    # outcome being hoped for.
    if [ "$code" = "137" ] && [ "$kind" = "-" ]; then
        survived=$((survived + 1))
        printf '  run %2d  survived %ss\n' "$i" "$SECONDS_PER_RUN"
    elif [ "$kind" != "-" ] && [ "$kind" != "(no report)" ]; then
        crashed=$((crashed + 1))
        printf '  run %2d  CRASH  exit %-4s %s\n' "$i" "$code" "$kind"
    else
        other=$((other + 1))
        printf '  run %2d  ended early, exit %-4s no crash block -- %s\n' "$i" "$code" "$kind"
    fi
done

echo
echo "  survived      $survived / $RUNS"
echo "  crashed       $crashed / $RUNS"
echo "  ended early   $other / $RUNS"
echo
echo "reports: $OUT"
