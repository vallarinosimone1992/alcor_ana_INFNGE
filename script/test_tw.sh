#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
root_dir="$(cd "${qa_dir}/.." && pwd)"

data_run="${root_dir}/data/golden_run"
pairs_file="${qa_dir}/config/coincidence_17_19.txt"

# Analysis options (edit if needed)
window_ns=20
duration_ns=20
fine_cut=0
clock_mhz=320
use_fine=1

fine_calib="${qa_dir}/calibration/fine_calibration.root"
chan_calib="${qa_dir}/calibration/channel_calibration.root"

run_coinc() {
  local label="$1"
  shift

  "${script_dir}/run_coincidence.sh" \
    -i "${data_run}" \
    -p "${pairs_file}" \
    -o "${label}" \
    -w "${window_ns}" \
    -d "${duration_ns}" \
    -F "${fine_cut}" \
    -m "${clock_mhz}" \
    -f "${use_fine}" \
    "$@"
}

echo "== Coincidence WITH time-walk correction"
run_coinc "golden_with_tw" -k "${fine_calib}" -K "${chan_calib}"

echo "== Coincidence WITHOUT time-walk correction"
run_coinc "golden_no_tw" -k "${fine_calib}" --no-chan-calib
