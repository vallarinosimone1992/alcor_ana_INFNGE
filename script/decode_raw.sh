#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: decode_raw.sh raw_run_dir_or_root [--force]

Scans for raw directories and decodes alcdaq.fifo_*.dat into ROOT files.

Input directory can be:
  - a run directory under raw_data (contains */raw)
  - a device directory that contains raw
  - a raw directory itself

Environment:
      DECODER_BIN  path to decoder binary (default: ALCOR_ANA_GE/decoder/bin/decoder)
  DATA_ROOT    output root directory (default: ../data)
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
if [ "$#" -gt 0 ]; then
  if [ "$1" = "--force" ]; then
    force=true
  else
    echo "unknown option: $1" >&2
    exit 1
  fi
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

  out_dir="${data_root}/${run_name}/${device}/decoded"
  mkdir -p "${out_dir}"

  for dat in "${raw_dir}"/alcdaq.fifo_*.dat; do
    [ -e "${dat}" ] || continue
    base="$(basename "${dat}")"
    out="${out_dir}/${base%.dat}.root"
    if [ -s "${out}" ] && [ "${force}" = false ]; then
      echo "skipping existing ${out}"
      continue
    fi
    echo "decoding ${dat} -> ${out}"
    "${decoder_bin}" --input "${dat}" --output "${out}"
  done
done < <(find "${input_dir}" -type d -name raw -print0 2>/dev/null)

if [ "${found}" = false ]; then
  echo "no raw directories found under ${input_dir}" >&2
  exit 1
fi