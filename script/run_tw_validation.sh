#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_tw_validation.sh [options]

Runs TW validation on calibration and golden runs.

Optional options:
  --calib-input PATH       calibration decoded/run dir (default: ../data/calibration)
  --golden-input PATH      golden decoded/run dir (default: ../data/golden_run)
  -p, --pairs FILE         pairs configuration file (default: config/coincidence_17_19.txt)
  -w, --window NS          coincidence window (default: 10)
  -m, --clock MHz          clock frequency (default: 320)
  -f, --use-fine [0|1]     enable fine timing (default 1)
      --no-fine            disable fine timing
  -d, --duration NS        max ToT (ns), also ToT axis (default 15)
  -F, --fine-cut N         exclude hits with |fine - cut| <= N (default 0)
  -k, --calib FILE         fine calibration ROOT file (default: calibration/fine_calibration.root)
  -K, --chan-calib FILE    channel calibration ROOT file (default: calibration/channel_calibration.root)
      --use-lut [0|1]      enable LUT correction (default 1)
      --no-lut             disable LUT correction
  -h, --help               show this help
USAGE
}

calib_input="../data/calibration"
golden_input="../data/golden_run"
pairs_file="config/coincidence_17_19.txt"
window_ns=10
clock_mhz=320
use_fine=1
max_duration_ns=15
fine_cut=0
fine_calib_path=""
chan_calib_path=""
use_lut=1

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
    --calib-input)
      need_arg "$@"
      calib_input=${2:-}
      shift 2
      ;;
    --golden-input)
      need_arg "$@"
      golden_input=${2:-}
      shift 2
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
    -w|--window)
      need_arg "$@"
      window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      window_ns=${1#*=}
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
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_fine=$2
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
    -d|--duration)
      need_arg "$@"
      max_duration_ns=${2:-}
      shift 2
      ;;
    --duration=*)
      max_duration_ns=${1#*=}
      shift
      ;;
    -F|--fine-cut)
      need_arg "$@"
      fine_cut=${2:-}
      shift 2
      ;;
    --fine-cut=*)
      fine_cut=${1#*=}
      shift
      ;;
    -k|--calib)
      need_arg "$@"
      fine_calib_path=${2:-}
      shift 2
      ;;
    --calib=*)
      fine_calib_path=${1#*=}
      shift
      ;;
    -K|--chan-calib)
      need_arg "$@"
      chan_calib_path=${2:-}
      shift 2
      ;;
    --chan-calib=*)
      chan_calib_path=${1#*=}
      shift
      ;;
    --use-lut)
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_lut=$2
        shift 2
      else
        use_lut=1
        shift
      fi
      ;;
    --use-lut=*)
      use_lut=${1#*=}
      shift
      ;;
    --no-lut)
      use_lut=0
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/tw_validation_rdf.cxx"
out_dir="${qa_dir}/output"

if [ -z "${fine_calib_path}" ]; then
  fine_calib_path="${qa_dir}/calibration/fine_calibration.root"
fi
if [ -z "${chan_calib_path}" ]; then
  chan_calib_path="${qa_dir}/calibration/channel_calibration.root"
fi

run_one() {
  local label="$1"
  local input_dir="$2"
  local out_pdf="${out_dir}/tw_validation_${label}.pdf"
  local log_path="${out_dir}/log_tw_validation_${label}_macro.txt"
  root -l -b -q "${macro_path}(\"${input_dir}\",\"${pairs_file}\",\"${out_pdf}\",${window_ns},${clock_mhz},${use_fine},${max_duration_ns},\"${fine_calib_path}\",\"${chan_calib_path}\",${fine_cut},${use_lut})" 2>&1 | tee "${log_path}"
}

mkdir -p "${out_dir}"

run_one "calibration" "${calib_input}"
run_one "golden" "${golden_input}"
