#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_analysis.sh --input PATH [options]

Run the standard analysis on decoded ALCOR data:
  1. per-channel plots
  2. coincidence plots/statistics for JSON-configured channel groups

Options:
  -i, --input PATH         decoded dir, run dir, or ROOT file
  -c, --config FILE       JSON analysis config (default: config/analysis.json)
  -l, --logbook FILE      JSON logbook (default: config/logbook.json)
      --channels LIST     override active channels (default: config/logbook)
  -o, --output-prefix STR output prefix (default: derived from input run)
  -k, --tdc-calib FILE    TDC calibration ROOT file (default: calibration/TDC_calibration.root)
      --calib FILE        alias for --tdc-calib
      --timewalk-calib FILE
                           timewalk correction ROOT file (default: calibration/timewalk_correction.root if present)
      --no-timewalk, --no-tw
                           disable timewalk correction only; fine calibration is still used
      --no-timewalk-calib compatibility alias for --no-timewalk
  -w, --window NS         override all coincidence windows
  -d, --duration NS       max ToT duration override
      --min-duration NS   minimum ToT duration override
  -m, --clock MHz         clock frequency override
  -f, --use-fine [0|1]    enable fine timing
      --no-fine           disable fine timing
      --no-lut            disable fine LUT
      --fine-cut N        exclude hits close to fine wrap cut
      --preview-hits N    print first N leading hits per channel
      --plot-only         run only per-channel plots
      --coincidence-only  run only coincidence analysis
      --dry-run           print commands without executing them
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
source "${script_dir}/lib/logbook.sh"

input=""
config_file="${qa_dir}/config/analysis.json"
logbook="$(logbook_default_path)"
channels_override=""
output_prefix=""
tdc_calib="${qa_dir}/calibration/TDC_calibration.root"
timewalk_calib=""
timewalk_set=0
window_override=""
duration_override=""
min_duration_override=""
clock_override=""
use_fine_override=""
use_lut_override=""
fine_cut_override=""
preview_hits=100
plot_only=0
coincidence_only=0
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
    -i|--input)
      need_arg "$@"
      input=${2:-}
      shift 2
      ;;
    --input=*)
      input=${1#*=}
      shift
      ;;
    -c|--config)
      need_arg "$@"
      config_file=${2:-}
      shift 2
      ;;
    --config=*)
      config_file=${1#*=}
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
    --channels)
      need_arg "$@"
      channels_override=${2:-}
      shift 2
      ;;
    --channels=*)
      channels_override=${1#*=}
      shift
      ;;
    -o|--output-prefix)
      need_arg "$@"
      output_prefix=${2:-}
      shift 2
      ;;
    --output-prefix=*)
      output_prefix=${1#*=}
      shift
      ;;
    -k|--tdc-calib|--calib)
      need_arg "$@"
      tdc_calib=${2:-}
      shift 2
      ;;
    --tdc-calib=*|--calib=*)
      tdc_calib=${1#*=}
      shift
      ;;
    --timewalk-calib)
      need_arg "$@"
      timewalk_calib=${2:-}
      timewalk_set=1
      shift 2
      ;;
    --timewalk-calib=*)
      timewalk_calib=${1#*=}
      timewalk_set=1
      shift
      ;;
    --no-timewalk|--no-tw|--skip-timewalk|--without-timewalk|--no-timewalk-calib)
      timewalk_calib=""
      timewalk_set=1
      shift
      ;;
    -w|--window)
      need_arg "$@"
      window_override=${2:-}
      shift 2
      ;;
    --window=*)
      window_override=${1#*=}
      shift
      ;;
    -d|--duration)
      need_arg "$@"
      duration_override=${2:-}
      shift 2
      ;;
    --duration=*)
      duration_override=${1#*=}
      shift
      ;;
    --min-duration)
      need_arg "$@"
      min_duration_override=${2:-}
      shift 2
      ;;
    --min-duration=*)
      min_duration_override=${1#*=}
      shift
      ;;
    -m|--clock)
      need_arg "$@"
      clock_override=${2:-}
      shift 2
      ;;
    --clock=*)
      clock_override=${1#*=}
      shift
      ;;
    -f|--use-fine)
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_fine_override=$2
        shift 2
      else
        use_fine_override=1
        shift
      fi
      ;;
    --use-fine=*)
      use_fine_override=${1#*=}
      shift
      ;;
    --no-fine)
      use_fine_override=0
      shift
      ;;
    --no-lut)
      use_lut_override=0
      shift
      ;;
    --fine-cut)
      need_arg "$@"
      fine_cut_override=${2:-}
      shift 2
      ;;
    --fine-cut=*)
      fine_cut_override=${1#*=}
      shift
      ;;
    --preview-hits)
      need_arg "$@"
      preview_hits=${2:-}
      shift 2
      ;;
    --preview-hits=*)
      preview_hits=${1#*=}
      shift
      ;;
    --plot-only)
      plot_only=1
      shift
      ;;
    --coincidence-only)
      coincidence_only=1
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

if [ -z "${input}" ]; then
  usage >&2
  exit 1
fi
if [ ! -f "${config_file}" ]; then
  echo "Analysis config not found: ${config_file}" >&2
  exit 1
fi
if [ "${plot_only}" -eq 1 ] && [ "${coincidence_only}" -eq 1 ]; then
  echo "--plot-only and --coincidence-only are mutually exclusive" >&2
  exit 1
fi
if [ "${dry_run}" -eq 0 ] && [ ! -s "${tdc_calib}" ]; then
  echo "TDC calibration not found: ${tdc_calib}" >&2
  exit 1
fi
if [ "${timewalk_set}" -eq 0 ] && [ -s "${qa_dir}/calibration/timewalk_correction.root" ]; then
  timewalk_calib="${qa_dir}/calibration/timewalk_correction.root"
fi

derive_output_base() {
  local path="${1%/}"
  local token=""
  IFS='/' read -r -a parts <<< "${path}"
  for part in "${parts[@]}"; do
    if [[ "${part}" =~ ^[0-9]{8}[-_][0-9]{6}$ ]]; then
      token="${part}"
    fi
  done
  if [ -z "${token}" ]; then
    token="$(basename "${path}")"
  fi
  token="${token//-/_}"
  echo "${token}"
}

run="$(logbook_run_from_path "${input}")"
logbook_channels=""
if [ -f "${logbook}" ] && logbook_has_run "${logbook}" "${run}"; then
  logbook_channels="$(logbook_channels_csv "${logbook}" "${run}")"
fi

if [ -z "${output_prefix}" ]; then
  output_prefix="$(derive_output_base "${input}")"
fi
output_prefix="${output_prefix%.pdf}"
output_dir="${qa_dir}/output"
tmp_dir="${output_dir}/.analysis_tmp"
mkdir -p "${output_dir}" "${tmp_dir}"

pairs_file="${tmp_dir}/${output_prefix}_coincidences.txt"
env_file="${tmp_dir}/${output_prefix}_analysis.env"

python3 - "${config_file}" "${pairs_file}" "${channels_override}" "${logbook_channels}" \
  "${window_override}" "${duration_override}" "${min_duration_override}" "${clock_override}" \
  "${use_fine_override}" "${use_lut_override}" "${fine_cut_override}" > "${env_file}" <<'PY'
import json
import shlex
import sys

config_path, pairs_path = sys.argv[1], sys.argv[2]
cli_channels, logbook_channels = sys.argv[3], sys.argv[4]
window_override, duration_override, min_duration_override = sys.argv[5], sys.argv[6], sys.argv[7]
clock_override, use_fine_override, use_lut_override, fine_cut_override = sys.argv[8], sys.argv[9], sys.argv[10], sys.argv[11]

with open(config_path, "r", encoding="utf-8") as handle:
    cfg = json.load(handle)

def channels_csv(value):
    if value is None:
        return ""
    if isinstance(value, str):
        if value.lower() == "logbook":
            return channels_csv(logbook_channels)
        cleaned = value.replace("_", " ").replace(",", " ")
        return ",".join(part for part in cleaned.split() if part)
    if isinstance(value, list):
        return ",".join(str(int(part)) for part in value)
    raise SystemExit(f"invalid channels value: {value!r}")

timing = cfg.get("timing", {})
plots = cfg.get("plots", {})

active_channels = channels_csv(cli_channels) if cli_channels else channels_csv(cfg.get("channels", "logbook"))
if not active_channels:
    raise SystemExit("cannot determine active channels; pass --channels or add the run to the logbook")

default_window = window_override or str(cfg.get("default_window_ns", 10))
max_duration = duration_override or str(timing.get("max_duration_ns", 15))
min_duration = min_duration_override or str(timing.get("min_duration_ns", 0))
clock = clock_override or str(timing.get("clock_mhz", 320))
use_fine = use_fine_override if use_fine_override else ("1" if timing.get("use_fine", True) else "0")
use_lut = use_lut_override if use_lut_override else ("1" if timing.get("use_lut", True) else "0")
fine_cut = fine_cut_override or str(timing.get("fine_cut", 0))
spill_index = str(plots.get("spill_index", 1))

coincidences = cfg.get("coincidences", [])
if not coincidences:
    channels = [int(ch) for ch in active_channels.split(",") if ch]
    if len(channels) >= 2:
        coincidences = [{"channels": channels, "window_ns": float(default_window)}]

with open(pairs_path, "w", encoding="utf-8") as out:
    for ch, value in cfg.get("duration_min_by_channel", {}).items():
        out.write(f"dmin {int(ch)} {float(value)}\n")
    for item in coincidences:
        channels = item.get("channels", [])
        if len(channels) < 2:
            continue
        window = item.get("window_ns", default_window)
        fields = [str(int(ch)) for ch in channels]
        if len(fields) == 2:
            out.write(f"{fields[0]} {fields[1]} window={float(window)}\n")
        else:
            out.write("group " + " ".join(fields) + f" window={float(window)}\n")

values = {
    "channels_csv": active_channels,
    "default_window_ns": default_window,
    "max_duration_ns": max_duration,
    "min_duration_ns": min_duration,
    "clock_mhz": clock,
    "use_fine": use_fine,
    "use_lut": use_lut,
    "fine_cut": fine_cut,
    "spill_index": spill_index,
}
for key, value in values.items():
    print(f"{key}={shlex.quote(str(value))}")
PY

source "${env_file}"

channels_pdf="${output_dir}/${output_prefix}_channels.pdf"
coinc_pdf="${output_dir}/${output_prefix}_coincidence.pdf"
coinc_root="${output_dir}/${output_prefix}_coincidence.root"
coinc_txt="${output_dir}/${output_prefix}_coincidence.txt"
plot_log="${output_dir}/log_${output_prefix}_channels_macro.txt"
coinc_log="${output_dir}/log_${output_prefix}_coincidence_macro.txt"

plot_macro="${qa_dir}/macro/plot_channels_rdf.cxx"
coinc_macro="${qa_dir}/macro/coincidence_rdf.cxx"
plot_cmd=(root -l -b -q "${plot_macro}(\"${input}\",\"${channels_pdf}\",\"${channels_csv}\",\"${tdc_calib}\",${spill_index},${fine_cut},${use_lut},${clock_mhz},${use_fine},${max_duration_ns},\"\")")
force_window=0
if [ -n "${window_override}" ]; then
  force_window=1
fi
coinc_cmd=(root -l -b -q "${coinc_macro}(\"${input}\",\"${pairs_file}\",\"${coinc_pdf}\",${default_window_ns},${clock_mhz},${use_fine},${max_duration_ns},${min_duration_ns},\"${tdc_calib}\",${force_window},\"\",${fine_cut},\"${coinc_root}\",\"${coinc_txt}\",${use_lut},${preview_hits},\"${timewalk_calib}\")")

echo "== Analysis input: ${input}"
echo "== Active channels: ${channels_csv}"
echo "== Coincidence config: ${config_file}"
echo "== Generated coincidence file: ${pairs_file}"
echo "== TDC calibration: ${tdc_calib}"
echo "== Timewalk calibration: ${timewalk_calib:-disabled}"

if [ "${dry_run}" -eq 1 ]; then
  if [ "${coincidence_only}" -eq 0 ]; then
    printf '%q ' "${plot_cmd[@]}"
    printf '\n'
  fi
  if [ "${plot_only}" -eq 0 ]; then
    printf '%q ' "${coinc_cmd[@]}"
    printf '\n'
  fi
  exit 0
fi

if [ "${coincidence_only}" -eq 0 ]; then
  "${plot_cmd[@]}" 2>&1 | tee "${plot_log}"
fi
if [ "${plot_only}" -eq 0 ]; then
  "${coinc_cmd[@]}" 2>&1 | tee "${coinc_log}"
fi
