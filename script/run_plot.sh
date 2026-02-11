#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_plot.sh [options]

Creates a multi-page PDF with per-channel distributions from decoded ROOT files.

Required options:
  -i, --input PATH         decoded dir, run dir, or parent dir
  -c, --channel CH         channel to plot (repeatable)
  -C, --channels LIST      comma- or space-separated channel list

Optional options:
  -o, --output NAME        output PDF name (default: derived from input dir)
  -s, --spill INDEX        spill index to plot (0-based), default 1 (second spill)
  -F, --fine-cut N         exclude hits with |fine - cut| <= N (default 0; 0 disables)
  -k, --calib FILE         fine calibration ROOT file (default: calibration/fine_calibration.root)
      --use-lut [0|1]      enable LUT correction (default 1)
      --no-lut             disable LUT correction (use original mapping)
      --config FILE        load defaults from config file (CLI overrides)
  -h, --help               show this help

Input directory can be:
  - a decoded directory containing alcdaq.fifo_*.root
  - a run directory that contains a decoded subdirectory
  - a parent directory with exactly one decoded folder within 3 levels
USAGE
}

out_name=""
input_dir=""
channels=()
fine_calib_path=""
spill_index=1
fine_cut=0
use_lut=1
config_file=""
cli_out_set=0
cli_input_set=0
cli_channels_set=0
cli_calib_set=0
cli_spill_set=0
cli_fine_cut_set=0
cli_use_lut_set=0

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

add_channels() {
  local list="${1//,/ }"
  for ch in $list; do
    channels+=("$ch")
  done
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    -o|--output)
      need_arg "$@"
      out_name=${2:-}
      cli_out_set=1
      shift 2
      ;;
    --output=*)
      out_name=${1#*=}
      cli_out_set=1
      shift
      ;;
    -i|--input)
      need_arg "$@"
      input_dir=${2:-}
      cli_input_set=1
      shift 2
      ;;
    --input=*)
      input_dir=${1#*=}
      cli_input_set=1
      shift
      ;;
    -c|--channel)
      need_arg "$@"
      add_channels "${2:-}"
      cli_channels_set=1
      shift 2
      ;;
    --channel=*)
      add_channels "${1#*=}"
      cli_channels_set=1
      shift
      ;;
    -C|--channels)
      need_arg "$@"
      add_channels "${2:-}"
      cli_channels_set=1
      shift 2
      ;;
    --channels=*)
      add_channels "${1#*=}"
      cli_channels_set=1
      shift
      ;;
    -s|--spill)
      need_arg "$@"
      spill_index=${2:-}
      cli_spill_set=1
      shift 2
      ;;
    --spill=*)
      spill_index=${1#*=}
      cli_spill_set=1
      shift
      ;;
    -F|--fine-cut)
      need_arg "$@"
      fine_cut=${2:-}
      cli_fine_cut_set=1
      shift 2
      ;;
    --fine-cut=*)
      fine_cut=${1#*=}
      cli_fine_cut_set=1
      shift
      ;;
    -k|--calib)
      need_arg "$@"
      fine_calib_path=${2:-}
      cli_calib_set=1
      shift 2
      ;;
    --calib=*)
      fine_calib_path=${1#*=}
      cli_calib_set=1
      shift
      ;;
    --use-lut)
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_lut=$2
        cli_use_lut_set=1
        shift 2
      else
        use_lut=1
        cli_use_lut_set=1
        shift
      fi
      ;;
    --use-lut=*)
      use_lut=${1#*=}
      cli_use_lut_set=1
      shift
      ;;
    --no-lut)
      use_lut=0
      cli_use_lut_set=1
      shift
      ;;
    --config)
      need_arg "$@"
      config_file=${2:-}
      shift 2
      ;;
    --config=*)
      config_file=${1#*=}
      shift
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      echo "Unexpected argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

parse_bool() {
  case "${1,,}" in
    1|true|yes|on) echo 1 ;;
    0|false|no|off) echo 0 ;;
    *) echo "$1" ;;
  esac
}

apply_config() {
  local file="$1"
  if [ ! -f "${file}" ]; then
    echo "config file not found: ${file}" >&2
    exit 1
  fi
  while IFS= read -r line; do
    line="${line%%#*}"
    line="${line%"${line##*[![:space:]]}"}"
    line="${line#"${line%%[![:space:]]*}"}"
    [ -z "${line}" ] && continue
    local key value
    if [[ "${line}" == *"="* ]]; then
      key="${line%%=*}"
      value="${line#*=}"
    else
      key="${line%%[[:space:]]*}"
      value="${line#"$key"}"
    fi
    key="${key,,}"
    value="${value#"${value%%[![:space:]]*}"}"
    value="${value%"${value##*[![:space:]]}"}"
    if [[ "${value}" == \"*\" && "${value}" == *\" ]]; then
      value="${value:1:${#value}-2}"
    fi
    case "${key}" in
      input)
        if [ "${cli_input_set}" -eq 0 ]; then input_dir="${value}"; fi
        ;;
      output)
        if [ "${cli_out_set}" -eq 0 ]; then out_name="${value}"; fi
        ;;
      channels|channel)
        if [ "${cli_channels_set}" -eq 0 ]; then
          channels=()
          add_channels "${value}"
        fi
        ;;
      spill|spill_index)
        if [ "${cli_spill_set}" -eq 0 ]; then spill_index="${value}"; fi
        ;;
      calib|calib_file|fine_calib)
        if [ "${cli_calib_set}" -eq 0 ]; then fine_calib_path="${value}"; fi
        ;;
      use_lut|lut)
        if [ "${cli_use_lut_set}" -eq 0 ]; then use_lut="$(parse_bool "${value}")"; fi
        ;;
      fine_cut|fine_cut_bins|fine_cut_distance)
        if [ "${cli_fine_cut_set}" -eq 0 ]; then fine_cut="${value}"; fi
        ;;
      *)
        echo "Ignoring unknown config key: ${key}" >&2
        ;;
    esac
  done < "${file}"
}

if [ -n "${config_file}" ]; then
  apply_config "${config_file}"
fi

if [ -z "${input_dir}" ] || [ "${#channels[@]}" -eq 0 ]; then
  usage >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/plot_channels_rdf.cxx"
out_dir="${qa_dir}/output"

if [ -z "${fine_calib_path}" ] && [ "${cli_calib_set}" -eq 0 ]; then
  fine_calib_path="${qa_dir}/calibration/fine_calibration.root"
fi

mkdir -p "${out_dir}"

has_root_files() {
  local dir="$1"
  compgen -G "${dir}/alcdaq.fifo_*.root" > /dev/null
}

derive_output_base() {
  local path="$1"
  if [ -f "${path}" ]; then
    path="$(dirname "${path}")"
  fi
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

resolved_dir="${input_dir}"
if [[ -f "${resolved_dir}" ]]; then
  :
elif ! has_root_files "${resolved_dir}"; then
  if [[ -d "${resolved_dir}/decoded" ]] && has_root_files "${resolved_dir}/decoded"; then
    resolved_dir="${resolved_dir}/decoded"
  else
    filtered=()
    while IFS= read -r cand; do
      if has_root_files "${cand}"; then
        filtered+=("${cand}")
      fi
    done < <(find "${resolved_dir}" -maxdepth 3 -type d -name decoded 2>/dev/null)
    if [[ "${#filtered[@]}" -eq 1 ]]; then
      resolved_dir="${filtered[0]}"
    else
      echo "No decoded ROOT files found under ${input_dir}" >&2
      echo "Pass a decoded directory such as .../kc705-196/decoded" >&2
      exit 1
    fi
  fi
fi

if [ -z "${out_name}" ]; then
  base="$(derive_output_base "${resolved_dir}")"
  out_name="${base}_plot"
fi

if [[ "${out_name}" != *.pdf ]]; then
  out_name="${out_name}.pdf"
fi

channels_csv=$(IFS=,; echo "${channels[*]}")
out_path="${out_dir}/${out_name}"
out_base="${out_name%.*}"
log_path="${out_dir}/log_${out_base}_macro.txt"

exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${resolved_dir}\",\"${out_path}\",\"${channels_csv}\",\"${fine_calib_path}\",${spill_index},${fine_cut},${use_lut})"
