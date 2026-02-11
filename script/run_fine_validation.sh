#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_fine_validation.sh [options]

Validates fine LUT using intrinsic and cross-validation tests.

Required options:
  -i, --input PATH         decoded dir, run dir, or parent dir

Optional options:
  -o, --output NAME        output PDF name (default: derived from input dir)
  -r, --root-out NAME      output ROOT name (default: output/fine_validation.root)
  -t, --txt-out NAME       output TXT name (default: output/fine_validation.txt)
  -e, --min-entries N      min entries per TDC index (default 200)
  -h, --help               show this help
USAGE
}

out_name=""
input_dir=""
out_root=""
out_txt=""
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
    -i|--input)
      need_arg "$@"
      input_dir=${2:-}
      shift 2
      ;;
    --input=*)
      input_dir=${1#*=}
      shift
      ;;
    -r|--root-out)
      need_arg "$@"
      out_root=${2:-}
      shift 2
      ;;
    --root-out=*)
      out_root=${1#*=}
      shift
      ;;
    -t|--txt-out)
      need_arg "$@"
      out_txt=${2:-}
      shift 2
      ;;
    --txt-out=*)
      out_txt=${1#*=}
      shift
      ;;
    -e|--min-entries)
      need_arg "$@"
      min_entries=${2:-}
      shift 2
      ;;
    --min-entries=*)
      min_entries=${1#*=}
      shift
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

if [ -z "${input_dir}" ]; then
  usage >&2
  exit 1
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
macro_path="${qa_dir}/macro/fine_validation_rdf.cxx"
out_dir="${qa_dir}/output"

mkdir -p "${out_dir}"

if [ -z "${out_name}" ]; then
  base="$(basename "${input_dir}")"
  base="${base//-/_}"
  out_name="${base}_fine_validation"
fi

if [[ "${out_name}" != *.pdf ]]; then
  out_name="${out_name}.pdf"
fi

out_pdf="${out_dir}/${out_name}"

if [ -z "${out_root}" ]; then
  out_root="${out_dir}/fine_validation.root"
fi
if [ -z "${out_txt}" ]; then
  out_txt="${out_dir}/fine_validation.txt"
fi

log_path="${out_dir}/log_${out_name%.pdf}_macro.txt"
exec > >(tee "${log_path}") 2>&1

root -l -b -q "${macro_path}(\"${input_dir}\",\"${out_pdf}\",\"${out_root}\",\"${out_txt}\",${min_entries})"
