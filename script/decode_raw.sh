#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: decode_raw.sh raw_run_dir_or_root [--force] [--include-trigger-fifo]

Scans for raw directories and decodes alcdaq.fifo_*.dat into ROOT files.

Input directory can be:
  - a run directory under raw_data (contains */raw)
  - a device directory that contains raw
  - a raw directory itself

Environment:
      DECODER_BIN  path to decoder binary (default: ALCOR_ANA_GE/decoder/bin/decoder)
  DATA_ROOT    output root directory (default: ../data)

By default the script skips alcdaq.fifo_24.dat. That trigger FIFO is not needed
for the standard channel analysis and has a different payload path in some runs.
Pass --include-trigger-fifo to decode it explicitly.
USAGE
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

if [ "$#" -lt 1 ]; then
  usage >&2
  exit 1
fi

input_dir=$1
shift

force=false
include_trigger_fifo=false
if [ "$#" -gt 0 ]; then
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --force)
        force=true
        shift
        ;;
      --include-trigger-fifo)
        include_trigger_fifo=true
        shift
        ;;
      *)
        echo "unknown option: $1" >&2
        exit 1
        ;;
    esac
  done
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
root_dir="$(cd "${qa_dir}/.." && pwd)"


decoder_bin="${DECODER_BIN:-${qa_dir}/decoder/bin/decoder}"
data_root="${DATA_ROOT:-${root_dir}/data}"

if [ ! -x "${decoder_bin}" ]; then
  echo "decoder not found or not executable: ${decoder_bin}" >&2
  echo "set DECODER_BIN or build the decoder with script/build_decoder.sh" >&2
  exit 1
fi

echo "Using decoder: ${decoder_bin}"

if [ ! -d "${input_dir}" ]; then
  echo "input path not found: ${input_dir}" >&2
  exit 1
fi

found=false
while IFS= read -r -d '' raw_dir; do
  found=true
  device_dir="$(dirname "${raw_dir}")"
  device="$(basename "${device_dir}")"
  run_dir="$(dirname "${device_dir}")"
  run_name="$(basename "${run_dir}")"

  link_path="${data_root}/${run_name}"
  if [ -L "${link_path}" ]; then
    target_path="$(python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "${link_path}")"
    if [ -n "${target_path}" ] && [ ! -d "${target_path}" ]; then
      mkdir -p "${target_path}"
    fi
  fi

  out_dir="${data_root}/${run_name}/${device}/decoded"
  mkdir -p "${out_dir}"

  for dat in "${raw_dir}"/alcdaq.fifo_*.dat; do
    [ -e "${dat}" ] || continue
    base="$(basename "${dat}")"
    if [ "${base}" = "alcdaq.fifo_24.dat" ] && [ "${include_trigger_fifo}" = false ]; then
      echo "skipping trigger FIFO ${dat} (use --include-trigger-fifo to decode it)"
      continue
    fi
    out="${out_dir}/${base%.dat}.root"
    if [ -s "${out}" ] && [ "${force}" = false ]; then
      echo "skipping existing ${out}"
      continue
    fi
    echo "decoding ${dat} -> ${out}"
    "${decoder_bin}" --input "${dat}" --output "${out}"
  done
done < <(find -L "${input_dir}" -type d -name raw -print0 2>/dev/null)

if [ "${found}" = false ]; then
  echo "no raw directories found under ${input_dir}" >&2
  exit 1
fi
