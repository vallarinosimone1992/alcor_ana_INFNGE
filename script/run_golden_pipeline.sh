#!/usr/bin/env bash
set -euo pipefail

# Pipeline: decode raw -> fine calibration -> channel calibration -> coincidence (calib + golden)

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

raw_calib="${base_dir}/raw_data/calibration"
raw_golden="${base_dir}/raw_data/golden_run"
data_calib="${base_dir}/data/calibration"
data_golden="${base_dir}/data/golden_run"

pairs_file="${qa_dir}/config/coincidence_17_19.txt"
fine_calib="${qa_dir}/calibration/fine_calibration.root"
chan_calib="${qa_dir}/calibration/channel_calibration.root"

duration_ns=30
window_ns=10
clock_mhz=320

echo "== Inputs (symlinks ok)"
ls -l "${raw_calib}" "${raw_golden}" "${data_calib}" "${data_golden}"

echo "== Decode raw (calibration)"
"${script_dir}/decode_raw.sh" "${raw_calib}" --force

echo "== Decode raw (golden run)"
"${script_dir}/decode_raw.sh" "${raw_golden}" --force

echo "== Fine calibration (matched to coincidence selection)"
"${script_dir}/run_fine_calibration.sh" \
  -i "${data_calib}" \
  -d "${duration_ns}" \
  --match-coincidence \
  -m "${clock_mhz}"

echo "== Channel calibration"
"${script_dir}/run_channel_calibration.sh" \
  -i "${data_calib}" \
  -k "${fine_calib}" \
  -d "${duration_ns}" \
  -w "${window_ns}" \
  -m "${clock_mhz}"

echo "== Coincidence (calibration, all calib on)"
"${script_dir}/run_coincidence.sh" \
  -i "${data_calib}" \
  -p "${pairs_file}" \
  -d "${duration_ns}" \
  -w "${window_ns}" \
  -k "${fine_calib}" \
  -K "${chan_calib}" \
  -o "calibration_with_calib"

echo "== Coincidence (golden run, all calib on)"
"${script_dir}/run_coincidence.sh" \
  -i "${data_golden}" \
  -p "${pairs_file}" \
  -d "${duration_ns}" \
  -w "${window_ns}" \
  -k "${fine_calib}" \
  -K "${chan_calib}" \
  -o "golden_with_calib"
