#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: run_tdc_calibration.sh --input PATH [--input PATH ...] [options]

Build the fine-TDC LUT calibration from decoded operation-mode-1 runs.
Each input is explicit: pass decoded directories, run directories, or ROOT files.
The script checks config/logbook.csv and requires Operating Mode 1 for every
input run unless --skip-logbook-check is used.

Options:
  -i, --input PATH         decoded dir, run dir, or ROOT file (repeatable)
  -l, --logbook FILE      CSV logbook (default: config/logbook.csv)
  -o, --output FILE       output ROOT file (default: calibration/TDC_calibration.root)
  -P, --pdf FILE          output PDF file (default: output/TDC_calibration.pdf)
      --skip-logbook-check
                          do not check Operating Mode in the logbook
      --q-low VALUE       low quantile for fine minimum (default: 0.01)
      --q-high VALUE      high quantile for fine maximum (default: 0.99)
      --min-entries N     minimum hits per TDC (default: 200)
      --match-coincidence build LUT from leading edges with valid ToT
      --no-match-coincidence
                          use all leading edges (default)
  -d, --duration NS       max ToT for --match-coincidence (default: 0 = disabled)
  -m, --clock MHz         clock frequency (default: 320)
      --dry-run           print commands without executing them
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
source "${script_dir}/lib/logbook.sh"
source "${script_dir}/lib/inputs.sh"

inputs=()
logbook="$(logbook_default_path)"
out_root="${qa_dir}/calibration/TDC_calibration.root"
out_pdf="${qa_dir}/output/TDC_calibration.pdf"
check_logbook=1
q_low=0.01
q_high=0.99
min_entries=200
match_coincidence=0
duration_ns=0
clock_mhz=320
dry_run=0

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
    -i|--input)
      need_arg "$@"
      inputs+=("${2:-}")
      shift 2
      ;;
    --input=*)
      inputs+=("${1#*=}")
      shift
      ;;
    -l|--logbook)
      need_arg "$@"
      logbook=${2:-}
      shift 2
      ;;
    --logbook=*)
      logbook=${1#*=}
      shift
      ;;
    -o|--output)
      need_arg "$@"
      out_root=${2:-}
      shift 2
      ;;
    --output=*)
      out_root=${1#*=}
      shift
      ;;
    -P|--pdf)
      need_arg "$@"
      out_pdf=${2:-}
      shift 2
      ;;
    --pdf=*)
      out_pdf=${1#*=}
      shift
      ;;
    --skip-logbook-check)
      check_logbook=0
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

if [ "${#inputs[@]}" -eq 0 ]; then
  usage >&2
  exit 1
fi

if [ "${check_logbook}" -eq 1 ] && [ ! -f "${logbook}" ]; then
  echo "Logbook not found: ${logbook}" >&2
  exit 1
fi

mkdir -p "$(dirname "${out_root}")" "$(dirname "${out_pdf}")" "${qa_dir}/output"
list_file="${qa_dir}/output/TDC_calibration_inputs.list"
log_path="${qa_dir}/output/log_TDC_calibration_macro.txt"

if [ "${check_logbook}" -eq 1 ]; then
  for input in "${inputs[@]}"; do
    run="$(logbook_run_from_path "${input}")"
    logbook_require_mode "${logbook}" "${run}" 1
  done
fi

if [ "${dry_run}" -eq 1 ]; then
  printf ': > %q\n' "${list_file}"
  for input in "${inputs[@]}"; do
    printf '# resolve %q into %q\n' "${input}" "${list_file}"
  done
else
  inputs_write_root_list "${list_file}" "${inputs[@]}"
fi

macro_path="${qa_dir}/macro/TDC_calibration_rdf.cxx"
cmd=(root -l -b -q "${macro_path}(\"${list_file}\",\"${out_root}\",${q_low},${q_high},${min_entries},\"${out_pdf}\",${duration_ns},${match_coincidence},${clock_mhz})")

echo "== TDC calibration inputs:"
printf '   %s\n' "${inputs[@]}"
echo "== Input list: ${list_file}"
echo "== Output calibration: ${out_root}"
echo "== Output PDF: ${out_pdf}"
echo "== Logbook check: ${check_logbook}"
echo "== Quantiles: ${q_low}, ${q_high}"
echo "== Min entries per TDC: ${min_entries}"
echo "== Match coincidence/ToT: ${match_coincidence}"

if [ "${dry_run}" -eq 1 ]; then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

"${cmd[@]}" 2>&1 | tee "${log_path}"

if [ ! -s "${out_root}" ]; then
  echo "TDC calibration output was not produced: ${out_root}" >&2
  exit 1
fi
