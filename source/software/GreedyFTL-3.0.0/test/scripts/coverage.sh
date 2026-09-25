#!/usr/bin/env bash
# Run the GreedyFTL host tests and produce an lcov summary + HTML report.
#
# Usage: coverage.sh <build-dir> <firmware-dir>
#
# Outputs (inside <build-dir>/coverage):
#   coverage.info  filtered lcov tracefile (firmware sources only)
#   summary.txt    `lcov --summary` output
#   per_file.txt   `lcov --list` output (one line per firmware file)
#   html/          genhtml report
set -euo pipefail

build_dir=$(cd "$1" && pwd)
fw_dir=$(cd "$2" && pwd)
out_dir="${build_dir}/coverage"
test_dir="${fw_dir}/test"

mkdir -p "${out_dir}"
rm -f "${out_dir}"/*.info

lcov --quiet --zerocounters --directory "${build_dir}"
lcov --quiet --capture --initial --directory "${build_dir}" \
  --base-directory "${fw_dir}" --output-file "${out_dir}/baseline.info"

test_status=0
(cd "${build_dir}" && ctest --output-on-failure) || test_status=$?

lcov --quiet --capture --directory "${build_dir}" \
  --base-directory "${fw_dir}" --output-file "${out_dir}/tests.info"
lcov --quiet --add-tracefile "${out_dir}/baseline.info" \
  --add-tracefile "${out_dir}/tests.info" --output-file "${out_dir}/total.info"
lcov --quiet --extract "${out_dir}/total.info" "${fw_dir}/*" \
  --output-file "${out_dir}/fw.info"
lcov --quiet --remove "${out_dir}/fw.info" "${test_dir}/*" \
  --output-file "${out_dir}/coverage.info"

lcov --summary "${out_dir}/coverage.info" 2>&1 | tee "${out_dir}/summary.txt"
lcov --list "${out_dir}/coverage.info" > "${out_dir}/per_file.txt"
cat "${out_dir}/per_file.txt"

genhtml --quiet --title "GreedyFTL-3.0.0 host tests" --legend \
  --prefix "${fw_dir}" --output-directory "${out_dir}/html" \
  "${out_dir}/coverage.info"
echo "HTML report: ${out_dir}/html/index.html"

exit "${test_status}"
