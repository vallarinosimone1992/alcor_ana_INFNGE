#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_channel_calibration.sh [options]

Builds per-channel timing calibration curves relative to a reference channel.

Required options:
  -i, --input PATH         decoded dir, run dir, or parent dir

Optional options:
  -o, --output NAME        output ROOT name (default: channel_calibration.root)
  -r, --ref CH             reference channel (default 19)
  -w, --window NS          coincidence window for matching (default 40)
  -d, --duration NS        max ToT (ns), also histogram range (default 20)
  -b, --bins N             ToT bins (default 60)
  -m, --clock MHz          clock frequency (default 320)
  -f, --use-fine [0|1]     enable fine timing (default 1)
      --no-fine            disable fine timing
  -k, --calib FILE         fine calibration ROOT file (default: calibration/fine_calibration.root)
      --symmetrize-ref     split correction between ref and channel (default on)
      --no-symmetrize-ref  disable symmetric split (ref is fixed)
      --use-lut [0|1]      enable LUT correction (default 1)
      --no-lut             disable LUT correction (use original mapping)
  -h, --help               show this help
USAGE
}

out_name=""
input_dir=""
ref_channel=19
window_ns=40
max_duration_ns=20
tot_bins=60
clock_mhz=320
use_fine=1
fine_calib_path=""
use_lut=1
symmetrize_ref=1

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
      shift 2
      ;;
    --output=*)
      out_name=${1#*=}
      shift
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
    -r|--ref)
      need_arg "$@"
      ref_channel=${2:-}
      shift 2
      ;;
    --ref=*)
      ref_channel=${1#*=}
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
      max_duration_ns=${2:-}
      shift 2
      ;;
    --duration=*)
      max_duration_ns=${1#*=}
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
    -k|--calib)
      need_arg "$@"
      fine_calib_path=${2:-}
      shift 2
      ;;
    --calib=*)
      fine_calib_path=${1#*=}
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
    --symmetrize-ref)
      symmetrize_ref=1
      shift
      ;;
    --no-symmetrize-ref)
      symmetrize_ref=0
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -z "${input_dir}" ]; then
  usage >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/channel_calibration_rdf.cxx"
out_dir="${qa_dir}/calibration"

if [ -z "${fine_calib_path}" ]; then
  fine_calib_path="${qa_dir}/calibration/fine_calibration.root"
fi

mkdir -p "${out_dir}"

has_root_files() {
  local dir="$1"
  compgen -G "${dir}/alcdaq.fifo_*.root" > /dev/null
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
  out_name="channel_calibration.root"
fi

if [[ "${out_name}" != *.root ]]; then
  out_name="${out_name}.root"
fi

out_path="${out_dir}/${out_name}"
out_base="${out_name%.*}"
log_path="${qa_dir}/output/log_${out_base}_macro.txt"
out_pdf="${qa_dir}/output/${out_base}.pdf"

exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${resolved_dir}\",\"${out_path}\",${ref_channel},${window_ns},${max_duration_ns},${tot_bins},${clock_mhz},${use_fine},\"${fine_calib_path}\",${use_lut},${symmetrize_ref},\"${out_pdf}\")"
