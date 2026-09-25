#!/bin/sh
# Report line coverage for the firmware sources compiled into the host test build.
# Uses lcov when installed, otherwise falls back to gcov's per-file summary.
set -e
FTL_DIR=$(cd "${FTL_DIR:-$(dirname "$0")/..}" && pwd -P)
BUILD_DIR=$(cd "${BUILD_DIR:-.}" && pwd -P)
GCDA_DIR="$BUILD_DIR/CMakeFiles/ftl_under_test.dir"

if command -v lcov >/dev/null 2>&1; then
	lcov --quiet --capture --directory "$BUILD_DIR" --output-file "$BUILD_DIR/coverage.info" \
	     --include "$FTL_DIR/*" --exclude "$FTL_DIR/test/*"
	lcov --list "$BUILD_DIR/coverage.info"
	exit 0
fi

echo "lcov not found; using gcov line summary"
cd "$GCDA_DIR"
total_exec=0; total_lines=0
for gcda in $(find . -name '*.gcda'); do
	out=$(gcov -n -o "$(dirname "$gcda")" "$gcda" 2>/dev/null)
	file=$(echo "$out" | sed -n "s/^File '\(.*\)'$/\1/p" | head -1)
	case "$file" in "$FTL_DIR"/*) ;; *) continue ;; esac
	summary=$(echo "$out" | sed -n 's/^Lines executed:\([0-9.]*\)% of \([0-9]*\)$/\1 \2/p' | head -1)
	pct=${summary% *}; lines=${summary#* }
	exec_lines=$(awk -v p="$pct" -v n="$lines" 'BEGIN{printf "%d", p*n/100+0.5}')
	total_exec=$((total_exec+exec_lines)); total_lines=$((total_lines+lines))
	printf '%-60s %6s%% (%s/%s lines)\n' "${file#$FTL_DIR/}" "$pct" "$exec_lines" "$lines"
done
awk -v e="$total_exec" -v n="$total_lines" 'BEGIN{printf "%-60s %6.1f%% (%d/%d lines)\n", "TOTAL (firmware sources in host build)", n?e*100/n:0, e, n}'
