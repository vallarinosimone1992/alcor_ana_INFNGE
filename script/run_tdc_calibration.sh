#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_tdc_calibration.sh [options]

Decode the raw run associated with raw_data/TDC_calibration, build the fine-TDC
LUT calibration, and update the stable calibration symlink
calibration/fine_calibration.root.

The default mode uses all leading-edge hits and does not require a matched ToT,
which is the intended mode for a Time-Of-Arrival-only TDC calibration run.

Options:
      --source-run RUN        update raw_data/TDC_calibration to point to RUN
                              (default run can be written as 20260521-113633)
  -R, --raw-input PATH        raw run or symlink to decode (default: raw_data/TDC_calibration)
  -D, --data-root DIR         decoded data root passed as DATA_ROOT (default: ../data)
  -o, --output NAME           output calibration ROOT name (default: fine_calibration_<label>.root)
      --calib-link FILE       stable symlink to update (default: calibration/fine_calibration.root)
      --no-link               do not update the stable calibration symlink
      --skip-decode           use already decoded data under DATA_ROOT
      --force-decode          pass --force to decode_raw.sh (default)
      --no-force-decode       keep existing decoded ROOT files when present
      --q-low VALUE           low quantile for min (default: 0.01)
      --q-high VALUE          high quantile for max (default: 0.99)
      --min-entries N         minimum hits per TDC (default: 200)
      --match-coincidence     build LUT from leading edges with valid ToT
      --no-match-coincidence  use all leading edges (default)
  -d, --duration NS           max ToT for --match-coincidence (default: 0 = disabled)
  -m, --clock MHz             clock frequency for ToT matching (default: 320)
      --dry-run               print commands without executing them
  -h, --help                  show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

raw_root="${base_dir}/raw_data"
data_root="${base_dir}/data"
raw_link_name="TDC_calibration"
raw_input=""
source_run=""
output_name=""
calib_link="${qa_dir}/calibration/fine_calibration.root"
update_link=1
do_decode=1
force_decode=1
dry_run=0

q_low=0.01
q_high=0.99
min_entries=200
match_coincidence=0
duration_ns=0
clock_mhz=320

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
    --source-run)
      need_arg "$@"
      source_run=${2:-}
      shift 2
      ;;
    --source-run=*)
      source_run=${1#*=}
      shift
      ;;
    -R|--raw-input)
      need_arg "$@"
      raw_input=${2:-}
      shift 2
      ;;
    --raw-input=*)
      raw_input=${1#*=}
      shift
      ;;
    -D|--data-root)
      need_arg "$@"
      data_root=${2:-}
      shift 2
      ;;
    --data-root=*)
      data_root=${1#*=}
      shift
      ;;
    -o|--output)
      need_arg "$@"
      output_name=${2:-}
      shift 2
      ;;
    --output=*)
      output_name=${1#*=}
      shift
      ;;
    --calib-link)
      need_arg "$@"
      calib_link=${2:-}
      shift 2
      ;;
    --calib-link=*)
      calib_link=${1#*=}
      shift
      ;;
    --no-link)
      update_link=0
      shift
      ;;
    --skip-decode)
      do_decode=0
      shift
      ;;
    --force-decode)
      force_decode=1
      shift
      ;;
    --no-force-decode)
      force_decode=0
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
    --match-coincidence)
      match_coincidence=1
      shift
      ;;
    --no-match-coincidence)
      match_coincidence=0
      shift
      ;;
    -d|--duration)
      need_arg "$@"
      duration_ns=${2:-}
      shift 2
      ;;
    --duration=*)
      duration_ns=${1#*=}
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

raw_link="${raw_root}/${raw_link_name}"
if [ -n "${source_run}" ]; then
  source_run="${source_run//_/-}"
  source_path="${raw_root}/${source_run}"
  if [ "${dry_run}" -eq 0 ] && [ ! -d "${source_path}" ]; then
    echo "Source raw run not found: ${source_path}" >&2
    exit 1
  fi
  if [ "${dry_run}" -eq 1 ]; then
    printf 'ln -sfn %q %q\n' "${source_run}" "${raw_link}"
  else
    ln -sfn "${source_run}" "${raw_link}"
  fi
  raw_input="${raw_link}"
fi

if [ -z "${raw_input}" ]; then
  raw_input="${raw_link}"
elif [[ "${raw_input}" != /* ]] && [ -e "${raw_root}/${raw_input}" ]; then
  raw_input="${raw_root}/${raw_input}"
fi

label="$(basename "${raw_input}")"
safe_label="${label//-/_}"
if [ -z "${output_name}" ]; then
  output_name="fine_calibration_${safe_label}.root"
fi
if [[ "${output_name}" == */* ]]; then
  echo "--output expects a file name, not a path. Use --calib-link to choose the stable link path." >&2
  exit 1
fi
if [[ "${output_name}" != *.root ]]; then
  output_name="${output_name}.root"
fi

calib_dir="${qa_dir}/calibration"
output_dir="${qa_dir}/output"
mkdir -p "${calib_dir}" "${output_dir}" "${data_root}" "$(dirname "${calib_link}")"

out_root="${calib_dir}/${output_name}"
out_base="$(basename "${out_root}")"
out_base="${out_base%.root}"
out_pdf="${output_dir}/${out_base}.pdf"

if [ "${dry_run}" -eq 0 ] && [ ! -d "${raw_input}" ]; then
  echo "Raw input not found: ${raw_input}" >&2
  exit 1
fi

decode_cmd=(env DATA_ROOT="${data_root}" "${script_dir}/decode_raw.sh" "${raw_input}")
if [ "${force_decode}" -eq 1 ]; then
  decode_cmd+=(--force)
fi

if [ "${do_decode}" -eq 1 ]; then
  echo "== Decode raw TDC input: ${raw_input}"
  if [ "${dry_run}" -eq 1 ]; then
    printf '%q ' "${decode_cmd[@]}"
    printf '\n'
  else
    "${decode_cmd[@]}"
  fi
fi

has_root_files() {
  local dir="$1"
  compgen -G "${dir}/alcdaq.fifo_*.root" > /dev/null
}

decoded_base="${data_root}/${label}"
decoded_dir=""
if [ "${dry_run}" -eq 0 ]; then
  if [[ -d "${decoded_base}/decoded" ]] && has_root_files "${decoded_base}/decoded"; then
    decoded_dir="${decoded_base}/decoded"
  else
    filtered=()
    while IFS= read -r cand; do
      if has_root_files "${cand}"; then
        filtered+=("${cand}")
      fi
    done < <(find -L "${decoded_base}" -maxdepth 3 -type d -name decoded 2>/dev/null)
    if [ "${#filtered[@]}" -eq 1 ]; then
      decoded_dir="${filtered[0]}"
    else
      echo "No unique decoded ROOT directory found under ${decoded_base}" >&2
      echo "Decode the TDC raw input first or check DATA_ROOT." >&2
      exit 1
    fi
  fi
else
  decoded_dir="${decoded_base}/kc705-196/decoded"
fi

calib_cmd=(
  "${script_dir}/run_fine_calibration.sh"
  -i "${decoded_dir}"
  -o "$(basename "${out_root}")"
  -P "$(basename "${out_pdf}")"
  --q-low "${q_low}"
  --q-high "${q_high}"
  --min-entries "${min_entries}"
  -d "${duration_ns}"
  -m "${clock_mhz}"
)
if [ "${match_coincidence}" -eq 1 ]; then
  calib_cmd+=(--match-coincidence)
else
  calib_cmd+=(--no-match-coincidence)
fi

echo "== Decoded input: ${decoded_dir}"
echo "== Output TDC calibration: ${out_root}"
echo "== Stable TDC symlink: ${calib_link}"
echo "== Quantiles: ${q_low}, ${q_high}"
echo "== Min entries per TDC: ${min_entries}"
echo "== Match coincidence/ToT: ${match_coincidence}"

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${calib_cmd[@]}"
  printf '\n'
  if [ "${update_link}" -eq 1 ]; then
    printf 'ln -sfn %q %q\n' "$(basename "${out_root}")" "${calib_link}"
  fi
  exit 0
fi

"${calib_cmd[@]}"

if [ ! -s "${out_root}" ]; then
  echo "TDC calibration output was not produced: ${out_root}" >&2
  exit 1
fi

if [ "${update_link}" -eq 1 ]; then
  link_dir="$(cd "$(dirname "${calib_link}")" && pwd)"
  out_dir="$(cd "$(dirname "${out_root}")" && pwd)"
  if [ "${link_dir}" = "${out_dir}" ]; then
    link_target="$(basename "${out_root}")"
  else
    link_target="${out_root}"
  fi
  ln -sfn "${link_target}" "${calib_link}"
  echo "== Updated TDC calibration symlink: ${calib_link} -> ${link_target}"
fi
