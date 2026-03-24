#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_all_calibrations.sh [options]

Runs the full calibration chain (fine + channel) on a calibration run.

Optional options:
  -i, --input PATH         decoded dir, run dir, or parent dir (default: ../data/calibration)
  -d, --duration NS        max ToT (ns) for channel calibration (default: 30)
  -w, --window NS          coincidence window for channel calibration (default: 40)
  -b, --bins N             ToT bins for channel calibration (default: 60)
  -m, --clock MHz          clock frequency (default: 320)
  -f, --use-fine [0|1]     enable fine timing for channel calibration (default 1)
      --no-fine            disable fine timing for channel calibration
  -h, --help               show this help
USAGE
}

input_dir="../data/calibration"
duration_ns=30
window_ns=40
tot_bins=60
clock_mhz=320
use_fine=1

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
    -i|--input)
      need_arg "$@"
      input_dir=${2:-}
      shift 2
      ;;
    --input=*)
      input_dir=${1#*=}
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
    -w|--window)
      need_arg "$@"
      window_ns=${2:-}
      shift 2
      ;;
    --window=*)
      window_ns=${1#*=}
      shift
      ;;
    -b|--bins)
      need_arg "$@"
      tot_bins=${2:-}
      shift 2
      ;;
    --bins=*)
      tot_bins=${1#*=}
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
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
fine_calib="${qa_dir}/calibration/fine_calibration.root"

echo "== Fine calibration"
"${script_dir}/run_fine_calibration.sh" \
  -i "${input_dir}" \
  -d "${duration_ns}" \
  --match-coincidence \
  -m "${clock_mhz}"

echo "== Channel calibration"
"${script_dir}/run_channel_calibration.sh" \
  -i "${input_dir}" \
  -k "${fine_calib}" \
  -d "${duration_ns}" \
  -w "${window_ns}" \
  -b "${tot_bins}" \
  -m "${clock_mhz}" \
  -f "${use_fine}"
