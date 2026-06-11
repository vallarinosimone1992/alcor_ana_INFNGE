#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: decode_raw.sh raw_run_dir_or_root [--force] [--include-trigger-fifo] [--fifo N] [--keep-going]

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

Use --fifo N to decode only one FIFO. The option can be repeated.
Use --keep-going to continue after a decoder failure and report the final status.
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
keep_going=false
selected_fifos=()
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
      --keep-going)
        keep_going=true
        shift
        ;;
      --fifo)
        if [ "$#" -lt 2 ] || [ -z "${2:-}" ]; then
          echo "missing value for --fifo" >&2
          exit 1
        fi
        selected_fifos+=("$2")
        shift 2
        ;;
      --fifo=*)
        selected_fifos+=("${1#*=}")
        shift
        ;;
      *)
        echo "unknown option: $1" >&2
        exit 1
        ;;
    esac
  done
fi

for fifo in "${selected_fifos[@]}"; do
  if ! [[ "${fifo}" =~ ^[0-9]+$ ]]; then
    echo "invalid FIFO number: ${fifo}" >&2
    exit 1
  fi
done

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
status=0
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

  echo "Raw dir: ${raw_dir}"
  echo "Output dir: ${out_dir}"

  fifo_list=()
  if [ "${#selected_fifos[@]}" -gt 0 ]; then
    fifo_list=("${selected_fifos[@]}")
  else
    for ((fifo = 0; fifo <= 24; ++fifo)); do
      fifo_list+=("${fifo}")
    done
  fi

  for fifo in "${fifo_list[@]}"; do
    dat="${raw_dir}/alcdaq.fifo_${fifo}.dat"
    if [ ! -e "${dat}" ]; then
      if [ "${#selected_fifos[@]}" -gt 0 ]; then
        echo "missing requested FIFO ${fifo}: ${dat}" >&2
      fi
      continue
    fi
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
    if [ -e "${out}" ] && [ "${force}" = true ]; then
      rm -f "${out}"
    fi
    echo "decoding ${dat} -> ${out}"
    if "${decoder_bin}" --input "${dat}" --output "${out}"; then
      :
    else
      rc=$?
      status="${rc}"
      echo "decoder failed for ${dat} with exit status ${rc}" >&2
      if [ "${keep_going}" = false ]; then
        exit "${rc}"
      fi
    fi
  done
done < <(find -L "${input_dir}" -type d -name raw -print0 2>/dev/null)

if [ "${found}" = false ]; then
  echo "no raw directories found under ${input_dir}" >&2
  exit 1
fi

exit "${status}"
