#!/usr/bin/env bash
# Collect gcov data from a host-test build directory and produce
#   <build>/coverage/firmware.info   lcov tracefile restricted to firmware sources
#   <build>/coverage/summary.txt     `lcov --summary` output
#   <build>/coverage/html/index.html HTML report (unless --no-html)
#
# usage: coverage.sh <build-dir> <firmware-dir> [--no-html]
set -euo pipefail

BUILD_DIR=$(cd "$1" && pwd)
FW_DIR=$(cd "$2" && pwd)
WANT_HTML=1
[ "${3:-}" = "--no-html" ] && WANT_HTML=0

LCOV=${LCOV:-lcov}
GENHTML=${GENHTML:-genhtml}
COV_DIR="$BUILD_DIR/coverage"
mkdir -p "$COV_DIR"

# lcov 1.x prints noisy perl "Subroutine ... redefined" warnings on Ubuntu.
quiet() { "$@" 2> >(grep -v 'redefined at' >&2 || true); }

quiet "$LCOV" --quiet --capture --directory "$BUILD_DIR" \
    --rc lcov_branch_coverage=1 --output-file "$COV_DIR/all.info"
quiet "$LCOV" --quiet --extract "$COV_DIR/all.info" "$FW_DIR/*" \
    --rc lcov_branch_coverage=1 --output-file "$COV_DIR/fw.info"
quiet "$LCOV" --quiet --remove "$COV_DIR/fw.info" "$FW_DIR/test/*" \
    --rc lcov_branch_coverage=1 --output-file "$COV_DIR/firmware.info"

if [ "$WANT_HTML" = 1 ]; then
    quiet "$GENHTML" --quiet --branch-coverage --legend \
        --title "GreedyFTL-3.0.0 host tests" \
        --output-directory "$COV_DIR/html" "$COV_DIR/firmware.info"
fi

quiet "$LCOV" --list "$COV_DIR/firmware.info" --rc lcov_branch_coverage=1
echo
quiet "$LCOV" --summary "$COV_DIR/firmware.info" | tee "$COV_DIR/summary.txt"
[ "$WANT_HTML" = 1 ] && echo "HTML report: $COV_DIR/html/index.html"
exit 0
