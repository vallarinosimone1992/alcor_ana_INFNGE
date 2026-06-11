#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_all_calibrations.sh --tdc-input PATH [--tw-input PATH ...] [options]

Run the standard timing calibration chain:
  1. TDC calibration -> calibration/TDC_calibration.root
  2. Timewalk/ToT calibration -> calibration/timewalk_correction.root

Options:
      --tdc-input PATH    input for TDC calibration (repeatable)
      --tw-input PATH     input for timewalk/ToT calibration (repeatable)
      --tw-mode MODE      trigger or intensity (default: trigger)
      --channels LIST     channels for --tw-mode intensity
      --trigger CH        trigger/reference channel for --tw-mode trigger
      --sensors CSV       sensor channels for --tw-mode trigger
  -l, --logbook FILE      JSON logbook (default: config/logbook.json)
  -d, --duration NS       max ToT duration passed to both steps (default: 30)
  -m, --clock MHz         clock frequency (default: 320)
      --dry-run           print commands without executing them
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
source "${script_dir}/lib/logbook.sh"

tdc_inputs=()
tw_inputs=()
tw_mode="trigger"
channels=""
trigger_channel=22
sensor_channels="17,19"
logbook="$(logbook_default_path)"
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
    --tdc-input)
      need_arg "$@"
      tdc_inputs+=("${2:-}")
      shift 2
      ;;
    --tdc-input=*)
      tdc_inputs+=("${1#*=}")
      shift
      ;;
    --tw-input)
      need_arg "$@"
      tw_inputs+=("${2:-}")
      shift 2
      ;;
    --tw-input=*)
      tw_inputs+=("${1#*=}")
      shift
      ;;
    --tw-mode)
      need_arg "$@"
      tw_mode=${2:-}
      shift 2
      ;;
    --tw-mode=*)
      tw_mode=${1#*=}
      shift
      ;;
    --channels)
      need_arg "$@"
      channels=${2:-}
      shift 2
      ;;
    --channels=*)
      channels=${1#*=}
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
    -l|--logbook)
      need_arg "$@"
      logbook=${2:-}
      shift 2
      ;;
    --logbook=*)
      logbook=${1#*=}
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

if [ "${#tdc_inputs[@]}" -eq 0 ] || [ "${#tw_inputs[@]}" -eq 0 ]; then
  usage >&2
  exit 1
fi

tdc_cmd=("${script_dir}/run_tdc_calibration.sh" --logbook "${logbook}" --duration "${duration_ns}" --clock "${clock_mhz}")
for input in "${tdc_inputs[@]}"; do
  tdc_cmd+=(--input "${input}")
done

tw_cmd=("${script_dir}/run_timewalk_calibration.sh" --mode "${tw_mode}" --logbook "${logbook}" --duration "${duration_ns}" --clock "${clock_mhz}")
for input in "${tw_inputs[@]}"; do
  tw_cmd+=(--input "${input}")
done
if [ -n "${channels}" ]; then
  tw_cmd+=(--channels "${channels}")
fi
tw_cmd+=(--trigger "${trigger_channel}" --sensors "${sensor_channels}")

if [ "${dry_run}" -eq 1 ]; then
  tdc_cmd+=(--dry-run)
  tw_cmd+=(--dry-run)
fi

echo "== TDC calibration"
"${tdc_cmd[@]}"

echo "== Timewalk/ToT calibration"
"${tw_cmd[@]}"
