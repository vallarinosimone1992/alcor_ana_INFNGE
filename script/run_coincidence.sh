#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_coincidence.sh [options]

Computes time-difference histograms for channel pairs and evaluates group coincidences.

Required options:
  -i, --input PATH         decoded dir, run dir, or parent dir
  -p, --pairs FILE         pairs configuration file

Optional options:
  -o, --output NAME        output PDF name (default: derived from input dir)
  -R, --root-out FILE      output ROOT name (default: same as PDF but .root)
      --no-root            disable ROOT output
  -T, --txt-out FILE       output TXT name (default: same as PDF but .txt)
      --no-txt             disable TXT output
  -L, --use-lut [0|1]      enable LUT from fine calibration (default 1)
      --no-lut             disable LUT (use linear min/max)
  -w, --window NS          default window (ns), default 10
  -m, --clock MHz          clock frequency (default 320)
  -f, --use-fine [0|1]     enable fine timing (default 1)
      --no-fine            disable fine timing
  -d, --duration NS        max leading-trailing duration (ns), default 15 (<=0 disables duration filter)
      --no-duration        disable duration filter (use leading edges only)
  -F, --fine-cut N         exclude hits with |fine - cut| <= N (default 0; 0 disables)
  -k, --calib FILE         fine calibration ROOT file (default: calibration/fine_calibration.root)
      --no-calib           disable fine calibration file (use default linear mapping)
  -K, --chan-calib FILE    channel calibration ROOT file (default: calibration/channel_calibration.root)
      --no-chan-calib      disable channel calibration file
      --config FILE        load defaults from config file (CLI overrides)
  -h, --help               show this help

Input directory can be:
  - a decoded directory containing alcdaq.fifo_*.root
  - a run directory that contains a decoded subdirectory
  - a parent directory with exactly one decoded folder within 3 levels
Spill markers (type==15) are used when spill is not provided (decoded input).
Pairs file can include group lines: group ch1 ch2 ch3 [window=ns]
USAGE
}

out_name=""
root_out=""
txt_out=""
input_dir=""
pairs_file=""
default_window_ns=10
clock_mhz=320
use_fine=1
max_duration_ns=15
fine_cut=0
use_lut=1
fine_calib_path=""
chan_calib_path=""
config_file=""
force_window=0
cli_out_set=0
cli_root_set=0
cli_txt_set=0
cli_input_set=0
cli_pairs_set=0
cli_window_set=0
cli_clock_set=0
cli_use_fine_set=0
cli_duration_set=0
cli_fine_cut_set=0
cli_use_lut_set=0
cli_calib_set=0
cli_chan_calib_set=0

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
    -R|--root-out)
      need_arg "$@"
      root_out=${2:-}
      cli_root_set=1
      shift 2
      ;;
    --root-out=*)
      root_out=${1#*=}
      cli_root_set=1
      shift
      ;;
    -T|--txt-out)
      need_arg "$@"
      txt_out=${2:-}
      cli_txt_set=1
      shift 2
      ;;
    --txt-out=*)
      txt_out=${1#*=}
      cli_txt_set=1
      shift
      ;;
    --no-root)
      root_out=""
      cli_root_set=1
      shift
      ;;
    --no-txt)
      txt_out=""
      cli_txt_set=1
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
    -p|--pairs)
      need_arg "$@"
      pairs_file=${2:-}
      cli_pairs_set=1
      shift 2
      ;;
    --pairs=*)
      pairs_file=${1#*=}
      cli_pairs_set=1
      shift
      ;;
    -w|--window)
      need_arg "$@"
      default_window_ns=${2:-}
      cli_window_set=1
      shift 2
      ;;
    --window=*)
      default_window_ns=${1#*=}
      cli_window_set=1
      shift
      ;;
    -m|--clock)
      need_arg "$@"
      clock_mhz=${2:-}
      cli_clock_set=1
      shift 2
      ;;
    --clock=*)
      clock_mhz=${1#*=}
      cli_clock_set=1
      shift
      ;;
    -f|--use-fine)
      if [ "${2:-}" = "0" ] || [ "${2:-}" = "1" ]; then
        use_fine=$2
        cli_use_fine_set=1
        shift 2
      else
        use_fine=1
        cli_use_fine_set=1
        shift
      fi
      ;;
    --use-fine=*)
      use_fine=${1#*=}
      cli_use_fine_set=1
      shift
      ;;
    --no-fine)
      use_fine=0
      cli_use_fine_set=1
      shift
      ;;
    -d|--duration)
      need_arg "$@"
      max_duration_ns=${2:-}
      cli_duration_set=1
      shift 2
      ;;
    --duration=*)
      max_duration_ns=${1#*=}
      cli_duration_set=1
      shift
      ;;
    --no-duration)
      max_duration_ns=0
      cli_duration_set=1
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
    -L|--use-lut)
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
    -k|--calib)
      need_arg "$@"
      fine_calib_path=${2:-}
      cli_calib_set=1
      shift 2
      ;;
    --no-calib)
      fine_calib_path=""
      cli_calib_set=1
      shift
      ;;
    -K|--chan-calib)
      need_arg "$@"
      chan_calib_path=${2:-}
      cli_chan_calib_set=1
      shift 2
      ;;
    --no-chan-calib)
      chan_calib_path=""
      cli_chan_calib_set=1
      shift
      ;;
    --calib=*)
      fine_calib_path=${1#*=}
      cli_calib_set=1
      shift
      ;;
    --chan-calib=*)
      chan_calib_path=${1#*=}
      cli_chan_calib_set=1
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
      root|root_out|root_file)
        if [ "${cli_root_set}" -eq 0 ]; then root_out="${value}"; fi
        ;;
      txt|txt_out|txt_file)
        if [ "${cli_txt_set}" -eq 0 ]; then txt_out="${value}"; fi
        ;;
      pairs|pairs_file)
        if [ "${cli_pairs_set}" -eq 0 ]; then pairs_file="${value}"; fi
        ;;
      window|window_ns)
        if [ "${cli_window_set}" -eq 0 ]; then default_window_ns="${value}"; fi
        ;;
      force_window|window_override)
        if [ "${cli_window_set}" -eq 0 ]; then
          force_window="$(parse_bool "${value}")"
        fi
        ;;
      clock|clock_mhz)
        if [ "${cli_clock_set}" -eq 0 ]; then clock_mhz="${value}"; fi
        ;;
      use_fine|fine)
        if [ "${cli_use_fine_set}" -eq 0 ]; then use_fine="$(parse_bool "${value}")"; fi
        ;;
      duration|max_duration)
        if [ "${cli_duration_set}" -eq 0 ]; then max_duration_ns="${value}"; fi
        ;;
      fine_cut|fine_cut_bins|fine_cut_distance)
        if [ "${cli_fine_cut_set}" -eq 0 ]; then fine_cut="${value}"; fi
        ;;
      use_lut|lut)
        if [ "${cli_use_lut_set}" -eq 0 ]; then use_lut="$(parse_bool "${value}")"; fi
        ;;
      calib|calib_file|fine_calib)
        if [ "${cli_calib_set}" -eq 0 ]; then fine_calib_path="${value}"; fi
        ;;
      chan_calib|channel_calib|channel_calibration)
        if [ "${cli_chan_calib_set}" -eq 0 ]; then chan_calib_path="${value}"; fi
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

if [ "${cli_window_set}" -eq 1 ]; then
  force_window=1
fi

if [ -z "${input_dir}" ] || [ -z "${pairs_file}" ]; then
  usage >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/coincidence_rdf.cxx"
out_dir="${qa_dir}/output"

if [ -z "${fine_calib_path}" ] && [ "${cli_calib_set}" -eq 0 ]; then
  fine_calib_path="${qa_dir}/calibration/fine_calibration.root"
fi
if [ -z "${chan_calib_path}" ] && [ "${cli_chan_calib_set}" -eq 0 ]; then
  chan_calib_path="${qa_dir}/calibration/channel_calibration.root"
fi

mkdir -p "${out_dir}"

if [ ! -f "${pairs_file}" ]; then
  echo "pairs file not found: ${pairs_file}" >&2
  exit 1
fi

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
    done < <(find -L "${resolved_dir}" -maxdepth 3 -type d -name decoded 2>/dev/null)
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
  out_name="${base}_coinc"
fi

if [[ "${out_name}" != *.pdf ]]; then
  out_name="${out_name}.pdf"
fi

out_path="${out_dir}/${out_name}"
out_base="${out_name%.*}"
if [ "${cli_root_set}" -eq 0 ]; then
  root_out="${out_dir}/${out_base}.root"
fi
if [ -n "${root_out}" ] && [[ "${root_out}" != *.root ]]; then
  root_out="${root_out}.root"
fi
if [ "${cli_txt_set}" -eq 0 ]; then
  txt_out="${out_dir}/${out_base}.txt"
fi
if [ -n "${txt_out}" ] && [[ "${txt_out}" != *.txt ]]; then
  txt_out="${txt_out}.txt"
fi
log_path="${out_dir}/log_${out_base}_macro.txt"

exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${resolved_dir}\",\"${pairs_file}\",\"${out_path}\",${default_window_ns},${clock_mhz},${use_fine},${max_duration_ns},\"${fine_calib_path}\",${force_window},\"${chan_calib_path}\",${fine_cut},\"${root_out}\",\"${txt_out}\",${use_lut})"
