#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_plot_lut.sh [options]

Plots the fine LUT (hFineLut) from a calibration ROOT file.

Optional options:
  -k, --calib FILE         fine calibration ROOT file (default: calibration/fine_calibration.root)
  -o, --output NAME        output PDF name (default: fine_lut.pdf)
  -f, --fifo N             select FIFO index (default: -1 = all)
  -i, --indices LIST       comma/space separated TDC indices for 1D slices
  -h, --help               show this help
USAGE
}

calib_path=""
out_name=""
fifo=-1
indices=""

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
    -k|--calib)
      need_arg "$@"
      calib_path=${2:-}
      shift 2
      ;;
    --calib=*)
      calib_path=${1#*=}
      shift
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
    -f|--fifo)
      need_arg "$@"
      fifo=${2:-}
      shift 2
      ;;
    --fifo=*)
      fifo=${1#*=}
      shift
      ;;
    -i|--indices)
      need_arg "$@"
      indices=${2:-}
      shift 2
      ;;
    --indices=*)
      indices=${1#*=}
      shift
      ;;
    -* )
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

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/plot_fine_lut.cxx"
out_dir="${qa_dir}/output"

mkdir -p "${out_dir}"

if [ -z "${calib_path}" ]; then
  calib_path="${qa_dir}/calibration/fine_calibration.root"
fi

if [ -z "${out_name}" ]; then
  out_name="fine_lut"
fi

if [[ "${out_name}" != *.pdf ]]; then
  out_name="${out_name}.pdf"
fi

out_pdf="${out_dir}/${out_name}"
log_path="${out_dir}/log_${out_name%.pdf}_macro.txt"

exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${calib_path}\",\"${out_pdf}\",${fifo},\"${indices}\")"
