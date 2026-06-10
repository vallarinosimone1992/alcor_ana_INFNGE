#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_logbook_two_channel_timewalk_coincidence.sh [options]

Run the standard coincidence analysis for the two-channel logbook runs, applying
the timewalk correction extracted from the laser-trigger analysis.

Options:
  -l, --logbook FILE        CSV logbook (default: GeLab OneDrive logbook)
  -D, --data-root DIR       decoded data root (default: ../data)
  -o, --output PREFIX       output filename prefix (default: logbook_17_19_timewalk)
      --channels VALUE      logbook Ch field to analyze (default: 17_19)
  -p, --pairs FILE          coincidence pairs file (default: config/coincidence_17_19.txt)
  -k, --calib FILE          fine calibration ROOT file (default: calibration/fine_calibration.root)
      --timewalk-calib FILE laser-analysis ROOT file with timewalk_corr_* parameters
  -K, --chan-calib FILE     optional channel calibration ROOT file
      --use-channel-calib   apply channel calibration in addition to timewalk correction (default off)
  -w, --window NS           coincidence window override passed to run_coincidence.sh (default: 20)
  -d, --duration NS         max leading-trailing duration / ToT range (default: 30)
  -m, --clock MHz           clock frequency (default: 320)
      --dry-run             print commands without executing them
  -h, --help                show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

logbook="/Users/simone/OneDrive - Istituto Nazionale di Fisica Nucleare/EIC/ePIC/dRICH/dRICH_Interaction_Tagger/GeLab_test/ALCOR_tests/ge_alcor_logbook.csv"
data_root="${base_dir}/data"
output_prefix="logbook_17_19_timewalk"
channels_filter="17_19"
pairs_file="${qa_dir}/config/coincidence_17_19.txt"
fine_calib="${qa_dir}/calibration/fine_calibration.root"
timewalk_calib=""
chan_calib=""
use_channel_calib=0
window_ns=20
duration_ns=30
clock_mhz=320
dry_run=0

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
      output_prefix=${2:-}
      shift 2
      ;;
    --output=*)
      output_prefix=${1#*=}
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
    -p|--pairs)
      need_arg "$@"
      pairs_file=${2:-}
      shift 2
      ;;
    --pairs=*)
      pairs_file=${1#*=}
      shift
      ;;
    -k|--calib)
      need_arg "$@"
      fine_calib=${2:-}
      shift 2
      ;;
    --calib=*)
      fine_calib=${1#*=}
      shift
      ;;
    --timewalk-calib)
      need_arg "$@"
      timewalk_calib=${2:-}
      shift 2
      ;;
    --timewalk-calib=*)
      timewalk_calib=${1#*=}
      shift
      ;;
    -K|--chan-calib)
      need_arg "$@"
      chan_calib=${2:-}
      use_channel_calib=1
      shift 2
      ;;
    --chan-calib=*)
      chan_calib=${1#*=}
      use_channel_calib=1
      shift
      ;;
    --use-channel-calib)
      use_channel_calib=1
      shift
      ;;
    -w|--window)
      need_arg "$@"
      window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      window_ns=${1#*=}
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

if [ -z "${timewalk_calib}" ]; then
  if [ -s "${qa_dir}/calibration/timewalk_correction.root" ]; then
    timewalk_calib="${qa_dir}/calibration/timewalk_correction.root"
  elif [ -s "${qa_dir}/output/test_ref22_145147_fine20260428.root" ]; then
    timewalk_calib="${qa_dir}/output/test_ref22_145147_fine20260428.root"
  else
    timewalk_calib="${qa_dir}/output/laser_logbook_17_19_22.root"
  fi
fi
if [ "${use_channel_calib}" -eq 1 ] && [ -z "${chan_calib}" ]; then
  chan_calib="${qa_dir}/calibration/channel_calibration_20260429_145147_ref22.root"
fi

if [ ! -f "${logbook}" ]; then
  echo "Logbook not found: ${logbook}" >&2
  exit 1
fi
if [ ! -d "${data_root}" ]; then
  echo "Data root not found: ${data_root}" >&2
  echo "Run script/decode_logbook_runs.sh --channels ${channels_filter} first." >&2
  exit 1
fi
if [ ! -f "${pairs_file}" ]; then
  echo "Pairs file not found: ${pairs_file}" >&2
  exit 1
fi
if [ "${dry_run}" -eq 0 ] && [ ! -s "${fine_calib}" ]; then
  echo "Fine calibration not found: ${fine_calib}" >&2
  exit 1
fi
if [ "${dry_run}" -eq 0 ] && [ ! -s "${timewalk_calib}" ]; then
  echo "Timewalk calibration not found: ${timewalk_calib}" >&2
  exit 1
fi
if [ "${dry_run}" -eq 0 ] && [ "${use_channel_calib}" -eq 1 ] && [ ! -s "${chan_calib}" ]; then
  echo "Channel calibration not found: ${chan_calib}" >&2
  exit 1
fi

data_root="$(cd "${data_root}" && pwd)"

echo "== Logbook: ${logbook}"
echo "== Data root: ${data_root}"
echo "== Pairs: ${pairs_file}"
echo "== Fine calibration: ${fine_calib}"
echo "== Timewalk calibration: ${timewalk_calib}"
if [ "${use_channel_calib}" -eq 1 ]; then
  echo "== Channel calibration: ${chan_calib}"
else
  echo "== Channel calibration: disabled"
fi

processed=0
while IFS=';' read -r name ch thr intensity spill vbias note rest || [ -n "${name:-}" ]; do
  name="${name#$'\xef\xbb\xbf'}"
  name="${name//$'\r'/}"
  ch="${ch//$'\r'/}"
  thr="${thr//$'\r'/}"
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

  out_name="${output_prefix}_${name}.pdf"
  cmd=("${script_dir}/run_coincidence.sh" \
    -i "${run_dir}" \
    -p "${pairs_file}" \
    -o "${out_name}" \
    -k "${fine_calib}" \
    --timewalk-calib "${timewalk_calib}" \
    -w "${window_ns}" \
    -d "${duration_ns}" \
    -m "${clock_mhz}" \
    --preview-hits 0)

  if [ "${use_channel_calib}" -eq 1 ]; then
    cmd+=(-K "${chan_calib}")
  else
    cmd+=(--no-chan-calib)
  fi

  echo "== Coincidence ${name} Ch=${ch} THR=${thr} I=${intensity}"
  if [ "${dry_run}" -eq 1 ]; then
    printf '%q ' "${cmd[@]}"
    printf '\n'
  else
    "${cmd[@]}"
  fi
  processed=$((processed + 1))
done < "${logbook}"

if [ "${processed}" -eq 0 ]; then
  echo "No runs processed for channels filter: ${channels_filter}" >&2
  exit 1
fi

echo "== Processed ${processed} runs"
