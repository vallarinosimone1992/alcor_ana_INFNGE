#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_fine_calibration.sh [options]

Builds fine time calibration histograms (hFineMin/hFineMax) from a calibration run.

Required options:
  -i, --input PATH         decoded dir, run dir, or parent dir

Optional options:
  -o, --output NAME        output ROOT name (default: fine_calibration.root, in calibration/)
  -P, --pdf-out NAME       output PDF name (default: output/<output>.pdf)
      --q-low VALUE        low quantile for min (default 0.01)
      --q-high VALUE       high quantile for max (default 0.99)
      --min-entries N      minimum hits per TDC (default 200)
  -h, --help               show this help
USAGE
}

out_name=""
pdf_out=""
input_dir=""
q_low=0.01
q_high=0.99
min_entries=200

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
    -P|--pdf-out)
      need_arg "$@"
      pdf_out=${2:-}
      shift 2
      ;;
    --pdf-out=*)
      pdf_out=${1#*=}
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
macro_path="${qa_dir}/macro/fine_calibration_rdf.cxx"
out_dir="${qa_dir}/calibration"
pdf_dir="${qa_dir}/output"

mkdir -p "${out_dir}" "${pdf_dir}"

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
  out_name="fine_calibration.root"
fi

if [[ "${out_name}" != *.root ]]; then
  out_name="${out_name}.root"
fi

out_path="${out_dir}/${out_name}"
out_base="${out_name%.*}"
log_path="${out_dir}/log_${out_base}_macro.txt"
if [ -z "${pdf_out}" ]; then
  pdf_path="${pdf_dir}/${out_base}.pdf"
else
  if [[ "${pdf_out}" != *.pdf ]]; then
    pdf_out="${pdf_out}.pdf"
  fi
  if [[ "${pdf_out}" == */* ]]; then
    pdf_path="${pdf_out}"
  else
    pdf_path="${pdf_dir}/${pdf_out}"
  fi
fi

exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${resolved_dir}\",\"${out_path}\",${q_low},${q_high},${min_entries},\"${pdf_path}\")"
