#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_tdc_calibration.sh --input PATH [--input PATH ...] [options]

Build the fine-TDC LUT calibration from decoded operation-mode-1 runs.
Each input is explicit: pass decoded directories, run directories, or ROOT files.
The script checks config/logbook.json and requires Operating Mode 1 for every
input run unless --skip-logbook-check is used.

Options:
  -i, --input PATH         decoded dir, run dir, or ROOT file (repeatable)
  -l, --logbook FILE      JSON logbook (default: config/logbook.json)
  -o, --output FILE       output ROOT file (default: calibration/TDC_calibration.root)
  -P, --pdf FILE          output PDF file (default: output/TDC_calibration.pdf)
      --skip-logbook-check
                          do not check Operating Mode in the logbook
      --q-low VALUE       low quantile for fine minimum (default: 0.01)
      --q-high VALUE      high quantile for fine maximum (default: 0.99)
      --min-entries N     minimum hits per TDC (default: 200)
      --match-coincidence build LUT from leading edges with valid ToT
      --no-match-coincidence
                          use all leading edges (default)
  -d, --duration NS       max ToT for --match-coincidence (default: 0 = disabled)
  -m, --clock MHz         clock frequency (default: 320)
      --offset-study      study relative channel/TDC offsets using calibrated TDC times (default)
      --no-offset-study   skip relative channel/TDC offset study
      --offset-reference event-median|CH
                          reference time for offsets (default: channel 22; event-median uses event medians)
      --offset-window NS  matching window for offset study (default: 100)
      --offset-event-window NS
                          event-building window for offset study (default: same as --offset-window)
      --offset-min-channels N
                          minimum channels per laser event in event-median mode (default: 3)
      --offset-channels logbook|CSV|all
                          channels to study for offsets (default: logbook)
      --dry-run           print commands without executing them
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
source "${script_dir}/lib/logbook.sh"
source "${script_dir}/lib/inputs.sh"
source "${script_dir}/lib/root_tools.sh"

inputs=()
logbook="$(logbook_default_path)"
out_root="${qa_dir}/calibration/TDC_calibration.root"
out_pdf="${qa_dir}/output/TDC_calibration.pdf"
check_logbook=1
q_low=0.01
q_high=0.99
min_entries=200
match_coincidence=0
duration_ns=0
clock_mhz=320
offset_study=1
offset_reference="22"
offset_window_ns=100
offset_event_window_ns=0
offset_min_channels=3
offset_channels="logbook"
dry_run=0

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

reject_directory_output() {
  local option="$1"
  local path="$2"
  if [[ "${path}" == */ ]] || [ -d "${path}" ]; then
    echo "${option} expects a file path, not a directory: ${path}" >&2
    exit 1
  fi
}

channels_to_csv() {
  local value="$1"
  python3 - "${value}" <<'PY'
import re
import sys

value = sys.argv[1]
out = []
seen = set()
for token in re.split(r"[,\s_]+", value.strip()):
    if not token:
        continue
    try:
        channel = int(token)
    except ValueError:
        raise SystemExit(f"invalid channel token: {token}")
    if channel < 0 or channel >= 32:
        raise SystemExit(f"channel out of range: {channel}")
    if channel in seen:
        continue
    seen.add(channel)
    out.append(str(channel))
print(",".join(out))
PY
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    -i|--input)
      need_arg "$@"
      inputs+=("${2:-}")
      shift 2
      ;;
    --input=*)
      inputs+=("${1#*=}")
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
    -o|--output)
      need_arg "$@"
      out_root=${2:-}
      reject_directory_output "$1" "${out_root}"
      shift 2
      ;;
    --output=*)
      out_root=${1#*=}
      reject_directory_output "--output" "${out_root}"
      shift
      ;;
    -P|--pdf)
      need_arg "$@"
      out_pdf=${2:-}
      reject_directory_output "$1" "${out_pdf}"
      shift 2
      ;;
    --pdf=*)
      out_pdf=${1#*=}
      reject_directory_output "--pdf" "${out_pdf}"
      shift
      ;;
    --skip-logbook-check)
      check_logbook=0
      shift
      ;;
    --q-low)
      need_arg "$@"
      q_low=${2:-}
      shift 2
      ;;
    --q-low=*)
      q_low=${1#*=}
      shift
      ;;
    --q-high)
      need_arg "$@"
      q_high=${2:-}
      shift 2
      ;;
    --q-high=*)
      q_high=${1#*=}
      shift
      ;;
    --min-entries)
      need_arg "$@"
      min_entries=${2:-}
      shift 2
      ;;
    --min-entries=*)
      min_entries=${1#*=}
      shift
      ;;
    --match-coincidence)
      match_coincidence=1
      shift
      ;;
    --no-match-coincidence)
      match_coincidence=0
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
    --offset-study|--study-offsets|--channel-offsets)
      offset_study=1
      shift
      ;;
    --no-offset-study|--no-study-offsets|--no-channel-offsets)
      offset_study=0
      shift
      ;;
    --offset-reference|--offset-ref|--reference-channel)
      need_arg "$@"
      offset_reference=${2:-}
      shift 2
      ;;
    --offset-reference=*|--offset-ref=*|--reference-channel=*)
      offset_reference=${1#*=}
      shift
      ;;
    --offset-window|--offset-match-window)
      need_arg "$@"
      offset_window_ns=${2:-}
      shift 2
      ;;
    --offset-window=*|--offset-match-window=*)
      offset_window_ns=${1#*=}
      shift
      ;;
    --offset-event-window|--offset-event-window-ns)
      need_arg "$@"
      offset_event_window_ns=${2:-}
      shift 2
      ;;
    --offset-event-window=*|--offset-event-window-ns=*)
      offset_event_window_ns=${1#*=}
      shift
      ;;
    --offset-min-channels|--offset-minimum-channels)
      need_arg "$@"
      offset_min_channels=${2:-}
      shift 2
      ;;
    --offset-min-channels=*|--offset-minimum-channels=*)
      offset_min_channels=${1#*=}
      shift
      ;;
    --offset-channels|--offset-channel-list|--channels-for-offset)
      need_arg "$@"
      offset_channels=${2:-}
      shift 2
      ;;
    --offset-channels=*|--offset-channel-list=*|--channels-for-offset=*)
      offset_channels=${1#*=}
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

if [ "${#inputs[@]}" -eq 0 ]; then
  usage >&2
  exit 1
fi

case "${offset_reference}" in
  event-median|event_median|median|event)
    offset_reference_arg=-1
    offset_reference_label="event-median"
    ;;
  *)
    offset_reference_arg=${offset_reference}
    offset_reference_label="channel ${offset_reference}"
    ;;
esac
if ! [[ "${offset_reference_arg}" =~ ^-?[0-9]+$ ]]; then
  echo "Invalid --offset-reference: ${offset_reference} (use event-median or a channel number)" >&2
  exit 1
fi
if ! [[ "${offset_min_channels}" =~ ^[0-9]+$ ]] || [ "${offset_min_channels}" -lt 1 ]; then
  echo "Invalid --offset-min-channels: ${offset_min_channels}" >&2
  exit 1
fi

if [ "${check_logbook}" -eq 1 ] && [ ! -f "${logbook}" ]; then
  echo "Logbook not found: ${logbook}" >&2
  exit 1
fi

offset_channels_arg=""
offset_channels_label="all"
case "${offset_channels}" in
  ""|all|All|ALL)
    offset_channels_arg=""
    offset_channels_label="all"
    ;;
  logbook|Logbook|LOGBOOK)
    inferred_channels=""
    missing_logbook_channels=0
    if [ -f "${logbook}" ]; then
      for input in "${inputs[@]}"; do
        run="$(logbook_run_from_path "${input}")"
        if logbook_has_run "${logbook}" "${run}"; then
          run_channels="$(logbook_channels_csv "${logbook}" "${run}")"
          if [ -n "${run_channels}" ]; then
            inferred_channels="${inferred_channels},${run_channels}"
          else
            missing_logbook_channels=1
          fi
        else
          missing_logbook_channels=1
        fi
      done
    else
      missing_logbook_channels=1
    fi
    offset_channels_arg="$(channels_to_csv "${inferred_channels#,}")"
    if [ -n "${offset_channels_arg}" ]; then
      offset_channels_label="logbook (${offset_channels_arg})"
    else
      offset_channels_label="all (logbook channels unavailable)"
      if [ "${offset_study}" -eq 1 ]; then
        echo "Warning: cannot infer offset channels from logbook; studying all channels." >&2
      fi
    fi
    ;;
  *)
    offset_channels_arg="$(channels_to_csv "${offset_channels}")"
    if [ -z "${offset_channels_arg}" ]; then
      echo "Invalid --offset-channels: ${offset_channels}" >&2
      exit 1
    fi
    offset_channels_label="${offset_channels_arg}"
    ;;
esac

mkdir -p "$(dirname "${out_root}")" "$(dirname "${out_pdf}")" "${qa_dir}/output"
list_file="${qa_dir}/output/TDC_calibration_inputs.list"
log_path="${qa_dir}/output/log_TDC_calibration_macro.txt"

if [ "${check_logbook}" -eq 1 ]; then
  for input in "${inputs[@]}"; do
    run="$(logbook_run_from_path "${input}")"
    logbook_require_mode "${logbook}" "${run}" 1
  done
fi

if [ "${dry_run}" -eq 1 ]; then
  printf ': > %q\n' "${list_file}"
  for input in "${inputs[@]}"; do
    printf '# resolve %q into %q\n' "${input}" "${list_file}"
  done
else
  inputs_write_root_list "${list_file}" "${inputs[@]}"
fi

tool_source="${qa_dir}/macro/run_tdc_calibration_main.cxx"
if [ "${dry_run}" -eq 1 ]; then
  tool_exe="$(root_tool_path "${qa_dir}" "run_tdc_calibration")"
else
  tool_exe="$(root_tool_build "${qa_dir}" "run_tdc_calibration" "${tool_source}")"
fi
cmd=("${tool_exe}" "${list_file}" "${out_root}" "${q_low}" "${q_high}" "${min_entries}" "${out_pdf}" "${duration_ns}" "${match_coincidence}" "${clock_mhz}" "${offset_study}" "${offset_reference_arg}" "${offset_window_ns}" "${offset_min_channels}" "${offset_channels_arg}" "${offset_event_window_ns}")

echo "== TDC calibration inputs:"
printf '   %s\n' "${inputs[@]}"
echo "== Input list: ${list_file}"
echo "== Output calibration: ${out_root}"
echo "== Output PDF: ${out_pdf}"
echo "== Logbook check: ${check_logbook}"
echo "== Quantiles: ${q_low}, ${q_high}"
echo "== Min entries per TDC: ${min_entries}"
echo "== Match coincidence/ToT: ${match_coincidence}"
echo "== Channel/TDC offset study: ${offset_study}"
echo "== Offset reference: ${offset_reference_label}"
echo "== Offset match window: ${offset_window_ns} ns"
if [ "${offset_event_window_ns}" = "0" ] || [ "${offset_event_window_ns}" = "0.0" ]; then
  echo "== Offset event window: same as offset match window"
else
  echo "== Offset event window: ${offset_event_window_ns} ns"
fi
echo "== Offset min channels: ${offset_min_channels}"
echo "== Offset channels: ${offset_channels_label}"

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

"${cmd[@]}" 2>&1 | tee "${log_path}"

if [ ! -s "${out_root}" ]; then
  echo "TDC calibration output was not produced: ${out_root}" >&2
  exit 1
fi
