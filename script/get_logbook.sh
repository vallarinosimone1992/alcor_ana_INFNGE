#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: get_logbook.sh [options]

Download the shared ALCOR logbook into config/logbook.csv.

Options:
  -o, --output FILE       output CSV path (default: config/logbook.csv)
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
url="https://docs.google.com/spreadsheets/d/1tWsHLoS6gbMfNyLN3z7DKl7RX0N3hX3YGDwn4ahgZuA/export?format=csv"
output="${qa_dir}/config/logbook.csv"

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
      output=${2:-}
      shift 2
      ;;
    --output=*)
      output=${1#*=}
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

mkdir -p "$(dirname "${output}")"
curl -fsSL "${url}" -o "${output}"
echo "Updated logbook: ${output}"
