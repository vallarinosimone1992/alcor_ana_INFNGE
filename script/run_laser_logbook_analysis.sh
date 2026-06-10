#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_laser_logbook_analysis.sh [options]

Analyze the GeLab logbook runs with a laser trigger on a selected reference
channel and SiPM signals on channels 17 and 19. The script builds a run list
from the CSV, optionally creates the fine-TDC LUT calibration, then runs the
ROOT macro that extracts absolute per-channel timewalk corrections versus the
reference trigger. Triggerless relative timewalk is intentionally not handled.

Options:
  -l, --logbook FILE          CSV logbook (default: GeLab OneDrive logbook)
  -D, --data-root DIR         decoded data root (default: ../data)
  -o, --output LABEL          output label (default: laser_logbook_17_19_22)
  -k, --calib FILE            fine calibration ROOT file
  -K, --chan-calib FILE       legacy channel calibration ROOT file
      --use-channel-calib     apply legacy channel/ToT calibration corrections (default off)
      --no-channel-calib      keep legacy channel/ToT calibration disabled
      --calibration-run RUN   run used to build calibration (default: 20260428_155514)
      --calib-ref CH          reference channel for legacy channel calibration (default: 19)
      --calib-window NS       coincidence window for legacy channel calibration (default: 20)
      --no-build-calibration  do not build missing calibration files
      --channels VALUE        logbook Ch field to analyze (default: 17_19_22)
      --trigger CH            laser trigger channel (default: 22)
      --sensors CSV           SiPM channels (default: 17,19)
  -w, --window NS             trigger matching window (default: 100)
      --trigger-deadtime NS   minimum separation for channel-22 trigger cleanup only (default: 50000)
      --trigger-period NS     expected valid channel-22 trigger period (default: 1000000)
      --trigger-period-tol NS tolerance for the trigger-period cleanup (default: 50000)
      --signed-dt             match to nearest trigger and keep signed time differences
      --timewalk-fit-ranges CSV
                              ToT fit ranges for accumulated timewalk fits (default: 17:7:18.5,19:3:15)
      --timewalk-fit-model MODEL
                              timewalk fit model: lin-exp-plateau or pol1 (default: lin-exp-plateau)
      --trigger-tot-window CSV
                              trigger/reference ToT selection window MIN:MAX, e.g. 1:3
      --spill-range CSV       inclusive spill selection range MIN:MAX
      --channel-tot-window CSV
                              per-channel ToT selection window CH:MIN:MAX[,CH:MIN:MAX]
      --edge-spill N          spill used for leading/trailing edge time maps
                              (default: first selected spill)
      --edge-channels CSV     channels for edge maps: all, analysis, or CSV (default: all)
      --edge-phase-period NS  period used to fold edge maps (default: trigger period)
      --edge-spill-fraction X initial spill fraction used by edge maps (default: 0.01)
  -d, --duration NS           max leading-trailing duration / ToT range (default: 30)
  -m, --clock MHz             clock frequency (default: 320)
  -f, --use-fine [0|1]        enable fine timing (default: 1)
      --no-fine               disable fine timing
      --no-lut                disable fine LUT
      --fine-cut N            exclude hits close to fine wrap cut (default: 0)
      --no-require-tot        also match leading hits without valid ToT
      --dry-run               print ROOT command without executing it
  -h, --help                  show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

logbook="/Users/simone/OneDrive - Istituto Nazionale di Fisica Nucleare/EIC/ePIC/dRICH/dRICH_Interaction_Tagger/GeLab_test/ALCOR_tests/ge_alcor_logbook.csv"
data_root="${base_dir}/data"
output_label="laser_logbook_17_19_22"
calibration_run="20260428_155514"
channels_filter="17_19_22"
trigger_channel=22
sensor_channels="17,19"
match_window_ns=100
trigger_deadtime_ns=50000
trigger_period_ns=1000000
trigger_period_tolerance_ns=50000
timewalk_fit_ranges="17:7:18.5,19:3:15"
timewalk_fit_model="lin-exp-plateau"
trigger_tot_window=""
spill_range=""
channel_tot_windows=""
edge_spill=-1
edge_channels="all"
edge_phase_period_ns=0
edge_spill_fraction=0.01
duration_ns=30
clock_mhz=320
use_fine=1
use_lut=1
fine_cut=0
require_tot=1
signed_dt=0
build_calibration=1
dry_run=0
fine_calib=""
chan_calib=""
fine_calib_set=0
chan_calib_set=0
use_channel_calib=0
calib_ref_channel=19
calib_window_ns=20

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    -l|--logbook)
      need_arg "$@"
      logbook=${2:-}
      shift 2
      ;;
    --logbook=*)
      logbook=${1#*=}
      shift
      ;;
    -D|--data-root)
      need_arg "$@"
      data_root=${2:-}
      shift 2
      ;;
    --data-root=*)
      data_root=${1#*=}
      shift
      ;;
    -o|--output)
      need_arg "$@"
      output_label=${2:-}
      shift 2
      ;;
    --output=*)
      output_label=${1#*=}
      shift
      ;;
    -k|--calib)
      need_arg "$@"
      fine_calib=${2:-}
      fine_calib_set=1
      shift 2
      ;;
    --calib=*)
      fine_calib=${1#*=}
      fine_calib_set=1
      shift
      ;;
    -K|--chan-calib)
      need_arg "$@"
      chan_calib=${2:-}
      chan_calib_set=1
      use_channel_calib=1
      shift 2
      ;;
    --chan-calib=*)
      chan_calib=${1#*=}
      chan_calib_set=1
      use_channel_calib=1
      shift
      ;;
    --no-channel-calib)
      chan_calib=""
      chan_calib_set=1
      use_channel_calib=0
      shift
      ;;
    --use-channel-calib)
      use_channel_calib=1
      shift
      ;;
    --calibration-run)
      need_arg "$@"
      calibration_run=${2:-}
      shift 2
      ;;
    --calibration-run=*)
      calibration_run=${1#*=}
      shift
      ;;
    --calib-ref|--calib-ref-channel)
      need_arg "$@"
      calib_ref_channel=${2:-}
      shift 2
      ;;
    --calib-ref=*|--calib-ref-channel=*)
      calib_ref_channel=${1#*=}
      shift
      ;;
    --calib-window)
      need_arg "$@"
      calib_window_ns=${2:-}
      shift 2
      ;;
    --calib-window=*)
      calib_window_ns=${1#*=}
      shift
      ;;
    --no-build-calibration)
      build_calibration=0
      shift
      ;;
    --channels)
      need_arg "$@"
      channels_filter=${2:-}
      shift 2
      ;;
    --channels=*)
      channels_filter=${1#*=}
      shift
      ;;
    --trigger)
      need_arg "$@"
      trigger_channel=${2:-}
      shift 2
      ;;
    --trigger=*)
      trigger_channel=${1#*=}
      shift
      ;;
    --sensors)
      need_arg "$@"
      sensor_channels=${2:-}
      shift 2
      ;;
    --sensors=*)
      sensor_channels=${1#*=}
      shift
      ;;
    -w|--window)
      need_arg "$@"
      match_window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      match_window_ns=${1#*=}
      shift
      ;;
    --trigger-deadtime)
      need_arg "$@"
      trigger_deadtime_ns=${2:-}
      shift 2
      ;;
    --trigger-deadtime=*)
      trigger_deadtime_ns=${1#*=}
      shift
      ;;
    --trigger-period)
      need_arg "$@"
      trigger_period_ns=${2:-}
      shift 2
      ;;
    --trigger-period=*)
      trigger_period_ns=${1#*=}
      shift
      ;;
    --trigger-period-tol|--trigger-period-tolerance)
      need_arg "$@"
      trigger_period_tolerance_ns=${2:-}
      shift 2
      ;;
    --trigger-period-tol=*|--trigger-period-tolerance=*)
      trigger_period_tolerance_ns=${1#*=}
      shift
      ;;
    --signed-dt)
      signed_dt=1
      shift
      ;;
    --timewalk-fit-ranges)
      need_arg "$@"
      timewalk_fit_ranges=${2:-}
      shift 2
      ;;
    --timewalk-fit-ranges=*)
      timewalk_fit_ranges=${1#*=}
      shift
      ;;
    --timewalk-fit-model)
      need_arg "$@"
      timewalk_fit_model=${2:-}
      shift 2
      ;;
    --timewalk-fit-model=*)
      timewalk_fit_model=${1#*=}
      shift
      ;;
    --trigger-tot-window|--trigger-tot-cut|--reference-tot-window|--ref-tot-window)
      need_arg "$@"
      trigger_tot_window=${2:-}
      shift 2
      ;;
    --trigger-tot-window=*|--trigger-tot-cut=*|--reference-tot-window=*|--ref-tot-window=*)
      trigger_tot_window=${1#*=}
      shift
      ;;
    --spill-range|--spills|--spill-window)
      need_arg "$@"
      spill_range=${2:-}
      shift 2
      ;;
    --spill-range=*|--spills=*|--spill-window=*)
      spill_range=${1#*=}
      shift
      ;;
    --channel-tot-window|--channel-tot-range|--tot-window-by-channel|--duration-window)
      need_arg "$@"
      channel_tot_windows=${2:-}
      shift 2
      ;;
    --channel-tot-window=*|--channel-tot-range=*|--tot-window-by-channel=*|--duration-window=*)
      channel_tot_windows=${1#*=}
      shift
      ;;
    --edge-spill|--edge-plot-spill)
      need_arg "$@"
      edge_spill=${2:-}
      shift 2
      ;;
    --edge-spill=*|--edge-plot-spill=*)
      edge_spill=${1#*=}
      shift
      ;;
    --edge-channels|--edge-plot-channels)
      need_arg "$@"
      edge_channels=${2:-}
      shift 2
      ;;
    --edge-channels=*|--edge-plot-channels=*)
      edge_channels=${1#*=}
      shift
      ;;
    --edge-phase-period|--edge-period)
      need_arg "$@"
      edge_phase_period_ns=${2:-}
      shift 2
      ;;
    --edge-phase-period=*|--edge-period=*)
      edge_phase_period_ns=${1#*=}
      shift
      ;;
    --edge-spill-fraction|--edge-fraction)
      need_arg "$@"
      edge_spill_fraction=${2:-}
      shift 2
      ;;
    --edge-spill-fraction=*|--edge-fraction=*)
      edge_spill_fraction=${1#*=}
      shift
      ;;
    -d|--duration)
      need_arg "$@"
      duration_ns=${2:-}
      shift 2
      ;;
    --duration=*)
      duration_ns=${1#*=}
      shift
      ;;
    -m|--clock)
      need_arg "$@"
      clock_mhz=${2:-}
      shift 2
      ;;
    --clock=*)
      clock_mhz=${1#*=}
      shift
      ;;
    -f|--use-fine)
      if [ "$#" -ge 2 ] && [[ "${2-}" =~ ^[01]$ ]]; then
        use_fine=${2}
        shift 2
      else
        use_fine=1
        shift
      fi
      ;;
    --use-fine=*)
      use_fine=${1#*=}
      shift
      ;;
    --no-fine)
      use_fine=0
      shift
      ;;
    --no-lut)
      use_lut=0
      shift
      ;;
    --fine-cut)
      need_arg "$@"
      fine_cut=${2:-}
      shift 2
      ;;
    --fine-cut=*)
      fine_cut=${1#*=}
      shift
      ;;
    --no-require-tot)
      require_tot=0
      shift
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ ! -f "${logbook}" ]; then
  echo "Logbook not found: ${logbook}" >&2
  exit 1
fi
if [ ! -d "${data_root}" ]; then
  echo "Data root not found: ${data_root}" >&2
  echo "Run script/decode_logbook_runs.sh first." >&2
  exit 1
fi

data_root="$(cd "${data_root}" && pwd)"
calibration_label="${calibration_run//-/_}"
calibration_dir="${calibration_run//_/-}"

if [ -z "${fine_calib}" ]; then
  fine_calib="${qa_dir}/calibration/fine_calibration_${calibration_label}.root"
fi
if [ "${use_channel_calib}" -eq 1 ] && [ -z "${chan_calib}" ]; then
  if [ "${calib_ref_channel}" = "19" ]; then
    chan_calib="${qa_dir}/calibration/channel_calibration_${calibration_label}.root"
  else
    chan_calib="${qa_dir}/calibration/channel_calibration_${calibration_label}_ref${calib_ref_channel}.root"
  fi
fi

mkdir -p "${qa_dir}/calibration" "${qa_dir}/output"

if [ "${build_calibration}" -eq 1 ] && [ "${dry_run}" -eq 0 ] &&
  { [ ! -s "${fine_calib}" ] || { [ "${use_channel_calib}" -eq 1 ] && [ ! -s "${chan_calib}" ]; }; }; then
  calib_input="${data_root}/${calibration_dir}"
  if [ ! -d "${calib_input}" ]; then
    echo "Missing decoded calibration input: ${calib_input}" >&2
    echo "Run script/decode_logbook_runs.sh first." >&2
    exit 1
  fi

  echo "== Building fine calibration from ${calibration_run}"
  fine_pdf="${fine_calib##*/}"
  fine_pdf="${fine_pdf%.root}.pdf"
  "${script_dir}/run_fine_calibration.sh" \
    -i "${calib_input}" \
    -o "${fine_calib##*/}" \
    -P "${fine_pdf}" \
    -d "${duration_ns}" \
    --match-coincidence \
    -m "${clock_mhz}"

  if [ "${fine_calib_set}" -eq 1 ] && [ ! -s "${fine_calib}" ]; then
    fine_calib="${qa_dir}/calibration/${fine_calib##*/}"
  fi

  if [ "${use_channel_calib}" -eq 1 ]; then
    echo "== Building channel calibration from ${calibration_run}"
    "${script_dir}/run_channel_calibration.sh" \
      -i "${calib_input}" \
      -k "${fine_calib}" \
      -o "${chan_calib##*/}" \
      -r "${calib_ref_channel}" \
      -d "${duration_ns}" \
      -w "${calib_window_ns}" \
      -m "${clock_mhz}" \
      -f "${use_fine}"

    if [ "${chan_calib_set}" -eq 1 ] && [ ! -s "${chan_calib}" ]; then
      chan_calib="${qa_dir}/calibration/${chan_calib##*/}"
    fi
  fi
fi

if [ "${dry_run}" -eq 0 ] && [ ! -s "${fine_calib}" ]; then
  echo "Fine calibration not found: ${fine_calib}" >&2
  exit 1
fi
if [ "${dry_run}" -eq 0 ] && [ "${use_channel_calib}" -eq 1 ] && [ ! -s "${chan_calib}" ]; then
  echo "Channel calibration not found: ${chan_calib}" >&2
  exit 1
fi

case "${output_label}" in
  /*)
    output_prefix="${output_label}"
    ;;
  */*)
    output_prefix="${qa_dir}/${output_label}"
    ;;
  *)
    output_prefix="${qa_dir}/output/${output_label}"
    ;;
esac

runlist="${output_prefix}_runlist.tsv"
out_pdf="${output_prefix}.pdf"
out_root="${output_prefix}.root"
out_txt="${output_prefix}.txt"
mkdir -p "$(dirname "${runlist}")" "$(dirname "${out_pdf}")" "$(dirname "${out_root}")" "$(dirname "${out_txt}")"

{
  printf '# run_label\tinput_path\tintensity\tchannels\tthresholds\tspill\tvbias\tnote\n'
  while IFS=';' read -r name ch thr intensity spill vbias note rest; do
    name="${name#$'\xef\xbb\xbf'}"
    name="${name//$'\r'/}"
    ch="${ch//$'\r'/}"
    thr="${thr//$'\r'/}"
    intensity="${intensity//$'\r'/}"
    spill="${spill//$'\r'/}"
    vbias="${vbias//$'\r'/}"
    note="${note//$'\r'/}"
    note="${note//$'\t'/ }"
    [ -z "${name}" ] && continue
    [ "${name}" = "Name" ] && continue
    if [ -n "${channels_filter}" ] && [ "${ch}" != "${channels_filter}" ]; then
      continue
    fi
    run_dir="${data_root}/${name//_/-}"
    if [ ! -d "${run_dir}" ]; then
      echo "Skipping missing decoded run: ${run_dir}" >&2
      continue
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "${name}" "${run_dir}" "${intensity}" "${ch}" "${thr}" "${spill}" "${vbias}" "${note}"
  done < "${logbook}"
} > "${runlist}"

if ! grep -q '^[^#[:space:]]' "${runlist}"; then
  echo "Run list is empty: ${runlist}" >&2
  exit 1
fi

macro_path="${qa_dir}/macro/laser_intensity_scan_rdf.cxx"
cmd=(root -l -b -q "${macro_path}(\"${runlist}\",\"${fine_calib}\",\"${chan_calib}\",\"${out_pdf}\",\"${out_root}\",\"${out_txt}\",${trigger_channel},\"${sensor_channels}\",${match_window_ns},${duration_ns},${clock_mhz},${use_fine},${use_lut},${fine_cut},${require_tot},${trigger_deadtime_ns},${trigger_period_ns},${trigger_period_tolerance_ns},${signed_dt},\"${timewalk_fit_ranges}\",\"${timewalk_fit_model}\",\"\",\"${trigger_tot_window}\",\"${spill_range}\",\"${channel_tot_windows}\",${edge_spill},\"${edge_channels}\",${edge_phase_period_ns},${edge_spill_fraction})")

echo "== Run list: ${runlist}"
echo "== Fine calibration: ${fine_calib}"
if [ "${use_channel_calib}" -eq 1 ]; then
  echo "== Legacy channel calibration: ${chan_calib}"
else
  echo "== Legacy channel calibration: disabled"
fi
echo "== Timewalk fit ranges: ${timewalk_fit_ranges}"
echo "== Timewalk fit model: ${timewalk_fit_model}"
echo "== Trigger ToT window: ${trigger_tot_window}"
echo "== Spill range: ${spill_range}"
echo "== Channel ToT windows: ${channel_tot_windows}"
echo "== Edge diagnostic spill: ${edge_spill}"
echo "== Edge diagnostic channels: ${edge_channels}"
echo "== Edge diagnostic phase period: ${edge_phase_period_ns}"
echo "== Edge diagnostic initial fraction: ${edge_spill_fraction}"
echo "== Output PDF: ${out_pdf}"

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
else
  "${cmd[@]}"
fi
