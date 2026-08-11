#!/usr/bin/env bash
# Dev workflow wrapper: clean/build/flash/monitor in one call, with the
# ESP-IDF python env pinned explicitly every run.
#
# Why this exists: idf.py auto-bootstraps its own environment on every
# invocation and has been observed resolving to a different python each time
# depending on which shell/tool invoked it (two ESP-IDF python installs exist
# on this machine: python_env/idf6.0_py3.11_env and tools/python/v6.0.2/venv).
# Whichever one configured the build "wins" until a fullclean, so plain
# idf.py commands run from different shells kept fighting each other with
# "python currently active differs from configured" errors. This script
# sources export.sh itself, once, from a fixed IDF_PATH, so every step in a
# single invocation uses the same python regardless of what the calling
# shell had active.
#
# Usage: tools/dev.sh -t <1.75|2.8c> -s <steps> [-p <port>]
#
# Steps (any combination of c/b/f/m, run in the order given):
#   c  fullclean the build dir
#   b  reconfigure + build
#   f  flash (requires -p)
#   m  monitor (requires -p)
#
# Examples:
#   tools/dev.sh -t 1.75 -s b
#   tools/dev.sh -t 1.75 -s bfm -p /dev/tty.usbmodem101
#   tools/dev.sh -t 2.8c -s cb
set -euo pipefail

usage() {
  sed -n '2,24p' "$0"
  exit 1
}

IDF_PATH_DEFAULT="/Users/david/.espressif/v6.0.2/esp-idf"
PORT=""
STEPS=""
TARGET=""

while getopts "t:s:p:h" opt; do
  case "$opt" in
    t) TARGET="$OPTARG" ;;
    s) STEPS="$OPTARG" ;;
    p) PORT="$OPTARG" ;;
    h) usage ;;
    *) usage ;;
  esac
done

[ -z "$TARGET" ] && usage
[ -z "$STEPS" ] && usage

case "$TARGET" in
  1.75|amoled|amoled_1_75) VARIANT="amoled_1_75" ;;
  2.8c|lcd|lcd_2_8c) VARIANT="lcd_2_8c" ;;
  *) echo "Unknown target: $TARGET (use 1.75 or 2.8c)"; exit 1 ;;
esac

BUILD_DIR="build-${VARIANT}"

: "${IDF_PATH:=$IDF_PATH_DEFAULT}"
export IDF_PATH
# shellcheck source=/dev/null
source "$IDF_PATH/export.sh" > /dev/null

cd "$(dirname "$0")/.."

for (( i=0; i<${#STEPS}; i++ )); do
  step="${STEPS:$i:1}"
  case "$step" in
    c)
      # rm -rf instead of `idf.py fullclean` — this project has repeatedly
      # hit stray "managed_components/<pkg> 2" duplicate dirs (debris from
      # overlapping idf.py runs) that make fullclean refuse to proceed as a
      # safety check. Both dirs are gitignored/fully disposable, so just
      # nuke them.
      echo "==> removing $BUILD_DIR and managed_components"
      rm -rf "$BUILD_DIR" managed_components
      ;;
    b)
      echo "==> build $VARIANT"
      idf.py -B "$BUILD_DIR" -DPRINTSPHERE_HW_VARIANT="$VARIANT" reconfigure build
      ;;
    f)
      [ -z "$PORT" ] && { echo "flash (f) requires -p <port>"; exit 1; }
      echo "==> flash $VARIANT via $PORT"
      idf.py -B "$BUILD_DIR" -DPRINTSPHERE_HW_VARIANT="$VARIANT" -p "$PORT" flash
      ;;
    m)
      [ -z "$PORT" ] && { echo "monitor (m) requires -p <port>"; exit 1; }
      echo "==> monitor $PORT"
      idf.py -B "$BUILD_DIR" -p "$PORT" monitor
      ;;
    *)
      echo "Unknown step '$step' (use any combo of c/b/f/m)"
      exit 1
      ;;
  esac
done
