#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
root_dir="$(cd "${qa_dir}/.." && pwd)"

raw_calib="${root_dir}/raw_data/calibration"
raw_golden="${root_dir}/raw_data/golden_run"

data_calib="${root_dir}/data/calibration"
data_golden="${root_dir}/data/golden_run"

pairs_file="${qa_dir}/config/coincidence_17_19.txt"

# Analysis options (edit here)
window_ns=20
duration_ns=30
fine_cut=0
clock_mhz=320
use_fine=1

fine_calib="${qa_dir}/calibration/fine_calibration.root"
chan_calib="${qa_dir}/calibration/channel_calibration.root"

echo "== Inputs (symlinks)"
ls -la "${raw_calib}" "${raw_golden}" "${data_calib}" "${data_golden}"

run_coinc() {
  local label="$1"
  local input="$2"
  shift 2

  "${script_dir}/run_coincidence.sh" \
    -i "${input}" \
    -p "${pairs_file}" \
    -o "${label}" \
    -w "${window_ns}" \
    -d "${duration_ns}" \
    -F "${fine_cut}" \
    -m "${clock_mhz}" \
    -f "${use_fine}" \
    "$@"
}

echo "== Decode raw (calibration)"
"${script_dir}/decode_raw.sh" "${raw_calib}" "--force" 

echo "== Decode raw (golden run)"
"${script_dir}/decode_raw.sh" "${raw_golden}" "--force"

echo "== Fine calibration (laser)"
"${script_dir}/run_fine_calibration.sh" -i "${data_calib}" -d "${duration_ns}" --match-coincidence

echo "== Channel calibration (laser)"
"${script_dir}/run_channel_calibration.sh" -i "${data_calib}" -k "${fine_calib}" -d "${duration_ns}"

echo "== Fine LUT validation (laser)"
"${script_dir}/run_fine_validation.sh" -i "${data_calib}"

echo "== TW validation (calibration + golden)"
"${script_dir}/run_tw_validation.sh" \
  --calib-input "${data_calib}" \
  --golden-input "${data_golden}" \
  -p "${pairs_file}" \
  -w "${window_ns}" \
  -d "${duration_ns}" \
  -F "${fine_cut}" \
  -m "${clock_mhz}" \
  -f "${use_fine}" \
  -k "${fine_calib}" \
  -K "${chan_calib}"

echo "== Coincidence (no calib)"
run_coinc "calibration_no_calib" "${data_calib}" --no-calib --no-chan-calib --no-lut
run_coinc "golden_no_calib" "${data_golden}" --no-calib --no-chan-calib --no-lut

echo "== Coincidence (no fine)"
run_coinc "calibration_no_fine" "${data_calib}" --no-fine --no-calib --no-chan-calib --no-lut
run_coinc "golden_no_fine" "${data_golden}" --no-fine --no-calib --no-chan-calib --no-lut

echo "== Coincidence (with calib)"
run_coinc "calibration_with_calib" "${data_calib}" -k "${fine_calib}" -K "${chan_calib}"
run_coinc "golden_with_calib" "${data_golden}" -k "${fine_calib}" -K "${chan_calib}"
