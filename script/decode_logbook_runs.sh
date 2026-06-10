#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: decode_logbook_runs.sh [options]

Decode, in series, the raw runs listed in the GeLab ALCOR CSV logbook.

Options:
  -l, --logbook FILE      CSV logbook (default: GeLab OneDrive logbook)
  -R, --raw-root DIR      raw data root (default: ../raw_data)
  -D, --data-root DIR     decoded data root passed as DATA_ROOT (default: ../data)
      --channels VALUE    only decode rows whose Ch field matches VALUE, e.g. 17_19_22
      --force             pass --force to decode_raw.sh
      --dry-run           print commands without executing them
  -h, --help              show this help

The logbook run names use YYYYMMDD_HHMMSS. Local raw/data directories use
YYYYMMDD-HHMMSS, so the script converts '_' to '-'.
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"

logbook="/Users/simone/OneDrive - Istituto Nazionale di Fisica Nucleare/EIC/ePIC/dRICH/dRICH_Interaction_Tagger/GeLab_test/ALCOR_tests/ge_alcor_logbook.csv"
raw_root="${base_dir}/raw_data"
data_root="${base_dir}/data"
channels_filter=""
force=0
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
    -l|--logbook)
      need_arg "$@"
      logbook=${2:-}
      shift 2
      ;;
    --logbook=*)
      logbook=${1#*=}
      shift
      ;;
    -R|--raw-root)
      need_arg "$@"
      raw_root=${2:-}
      shift 2
      ;;
    --raw-root=*)
      raw_root=${1#*=}
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
    --channels)
      need_arg "$@"
      channels_filter=${2:-}
      shift 2
      ;;
    --channels=*)
      channels_filter=${1#*=}
      shift
      ;;
    --force)
      force=1
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

if [ ! -f "${logbook}" ]; then
  echo "Logbook not found: ${logbook}" >&2
  exit 1
fi
if [ ! -d "${raw_root}" ]; then
  echo "Raw root not found: ${raw_root}" >&2
  exit 1
fi
mkdir -p "${data_root}"

raw_root="$(cd "${raw_root}" && pwd)"
data_root="$(cd "${data_root}" && pwd)"

decoded=0
missing=0
skipped=0

while IFS=';' read -r name ch thr intensity spill vbias note rest; do
  name="${name#$'\xef\xbb\xbf'}"
  name="${name//$'\r'/}"
  ch="${ch//$'\r'/}"
  [ -z "${name}" ] && continue
  [ "${name}" = "Name" ] && continue

  if [ -n "${channels_filter}" ] && [ "${ch}" != "${channels_filter}" ]; then
    skipped=$((skipped + 1))
    continue
  fi

  run_name="${name//_/-}"
  raw_run="${raw_root}/${run_name}"
  if [ ! -d "${raw_run}" ]; then
    echo "missing raw run: ${raw_run}" >&2
    missing=$((missing + 1))
    continue
  fi

  cmd=(env DATA_ROOT="${data_root}" "${script_dir}/decode_raw.sh" "${raw_run}")
  if [ "${force}" -eq 1 ]; then
    cmd+=(--force)
  fi

  printf '== %s  Ch=%s  laser=%s\n' "${name}" "${ch}" "${intensity}"
  if [ "${dry_run}" -eq 1 ]; then
    printf 'DATA_ROOT=%q %q %q' "${data_root}" "${script_dir}/decode_raw.sh" "${raw_run}"
    if [ "${force}" -eq 1 ]; then
      printf ' --force'
    fi
    printf '\n'
  else
    "${cmd[@]}"
  fi
  decoded=$((decoded + 1))
done < "${logbook}"

echo "Decoded/listed runs: ${decoded}"
echo "Skipped by channel filter: ${skipped}"
echo "Missing raw runs: ${missing}"

if [ "${missing}" -gt 0 ]; then
  exit 2
fi
