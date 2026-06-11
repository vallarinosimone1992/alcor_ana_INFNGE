#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: decode_logbook_runs.sh [options]

Decode, in series, the raw runs listed in config/logbook.json.

Options:
  -l, --logbook FILE      JSON logbook (default: config/logbook.json)
  -R, --raw-root DIR      raw data root (default: ../raw_data)
  -D, --data-root DIR     decoded data root passed as DATA_ROOT (default: ../data)
      --channels LIST     only decode rows whose Channels field matches LIST
      --mode VALUE        only decode rows whose Operating Mode values all match VALUE
      --force             pass --force to decode_raw.sh
      --dry-run           print commands without executing them
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
base_dir="$(cd "${qa_dir}/.." && pwd)"
source "${script_dir}/lib/logbook.sh"

logbook="$(logbook_default_path)"
raw_root="${base_dir}/raw_data"
data_root="${base_dir}/data"
channels_filter=""
mode_filter=""
force=0
dry_run=0

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

normalize_list() {
  local value="$1"
  value="${value//,/ }"
  value="${value//_/ }"
  awk -v value="${value}" 'BEGIN {
    n = split(value, a, /[[:space:]]+/)
    out = ""
    for (i = 1; i <= n; ++i) {
      if (a[i] == "") continue
      if (out != "") out = out " "
      out = out a[i]
    }
    print out
  }'
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
      channels_filter="$(normalize_list "${2:-}")"
      shift 2
      ;;
    --channels=*)
      channels_filter="$(normalize_list "${1#*=}")"
      shift
      ;;
    --mode)
      need_arg "$@"
      mode_filter=${2:-}
      shift 2
      ;;
    --mode=*)
      mode_filter=${1#*=}
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

while IFS= read -r date || [ -n "${date:-}" ]; do
  date="$(logbook_normalize_run "${date}")"
  [ -z "${date}" ] && continue
  channels="$(normalize_list "$(logbook_channels "${logbook}" "${date}")")"
  mode="$(normalize_list "$(logbook_modes "${logbook}" "${date}")")"
  intensity="$(logbook_field "${logbook}" "${date}" intensity)"

  if [ -n "${channels_filter}" ] && [ "${channels}" != "${channels_filter}" ]; then
    skipped=$((skipped + 1))
    continue
  fi
  if [ -n "${mode_filter}" ]; then
    if ! awk -v modes="${mode}" -v required="${mode_filter}" 'BEGIN {
      n = split(modes, a, /[[:space:]]+/)
      ok = 1
      seen = 0
      for (i = 1; i <= n; ++i) {
        if (a[i] == "") continue
        seen = 1
        if (a[i] != required) ok = 0
      }
      exit(seen && ok ? 0 : 1)
    }'; then
      skipped=$((skipped + 1))
      continue
    fi
  fi

  raw_run="${raw_root}/${date}"
  if [ ! -d "${raw_run}" ]; then
    echo "missing raw run: ${raw_run}" >&2
    missing=$((missing + 1))
    continue
  fi

  cmd=(env DATA_ROOT="${data_root}" "${script_dir}/decode_raw.sh" "${raw_run}")
  if [ "${force}" -eq 1 ]; then
    cmd+=(--force)
  fi

  printf '== %s  Channels=%s  mode=%s  laser=%s\n' "${date}" "${channels}" "${mode}" "${intensity}"
  if [ "${dry_run}" -eq 1 ]; then
    printf '%q ' "${cmd[@]}"
    printf '\n'
  else
    "${cmd[@]}"
  fi
  decoded=$((decoded + 1))
done < <(logbook_list_runs "${logbook}")

echo "Decoded/listed runs: ${decoded}"
echo "Skipped by filters: ${skipped}"
echo "Missing raw runs: ${missing}"

if [ "${missing}" -gt 0 ] && [ "${dry_run}" -eq 0 ]; then
  exit 2
fi
