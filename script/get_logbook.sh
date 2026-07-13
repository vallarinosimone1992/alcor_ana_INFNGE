#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: get_logbook.sh [options]

Download the shared ALCOR logbook into config/logbook.csv and also write a
normalized JSON copy for ROOT/C++ consumers.

Options:
  -o, --output FILE       output CSV path (default: config/logbook.csv)
  -j, --json-output FILE  output JSON path (default: CSV path with .json)
      --from-csv FILE     convert an existing CSV instead of downloading
      --no-json           only download the CSV
  -h, --help              show this help
USAGE
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"
url="https://docs.google.com/spreadsheets/d/1tWsHLoS6gbMfNyLN3z7DKl7RX0N3hX3YGDwn4ahgZuA/export?format=csv"
output="${qa_dir}/config/logbook.csv"
output_set=0
json_output=""
write_json=1
from_csv=""

need_arg() {
  if [ "$#" -lt 2 ] || [ -z "${2-}" ]; then
    echo "Missing value for $1" >&2
    usage >&2
    exit 1
  fi
}

default_json_output() {
  local csv_path="$1"
  if [[ "${csv_path}" == *.csv ]]; then
    printf '%s\n' "${csv_path%.csv}.json"
  else
    printf '%s\n' "${csv_path}.json"
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
      output_set=1
      shift 2
      ;;
    --output=*)
      output=${1#*=}
      output_set=1
      shift
      ;;
    -j|--json-output)
      need_arg "$@"
      json_output=${2:-}
      shift 2
      ;;
    --json-output=*)
      json_output=${1#*=}
      shift
      ;;
    --from-csv)
      need_arg "$@"
      from_csv=${2:-}
      shift 2
      ;;
    --from-csv=*)
      from_csv=${1#*=}
      shift
      ;;
    --no-json)
      write_json=0
      shift
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [ -n "${from_csv}" ]; then
  if [ "${output_set}" -eq 1 ]; then
    echo "--output cannot be used together with --from-csv; pass --json-output for the converted JSON path" >&2
    exit 1
  fi
  output="${from_csv}"
  if [ ! -s "${output}" ]; then
    echo "CSV logbook not found: ${output}" >&2
    exit 1
  fi
  echo "Using existing logbook CSV: ${output}"
else
  mkdir -p "$(dirname "${output}")"
  curl -fsSL "${url}" -o "${output}"
  echo "Updated logbook CSV: ${output}"
fi

if [ "${write_json}" -eq 1 ]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "CSV updated, but python3 is required to write the JSON logbook" >&2
    exit 1
  fi
  if [ -z "${json_output}" ]; then
    json_output="$(default_json_output "${output}")"
  fi
  mkdir -p "$(dirname "${json_output}")"
  python3 - "${output}" "${json_output}" <<'PY'
import csv
import json
import os
import re
import sys

csv_path, json_path = sys.argv[1], sys.argv[2]

def clean(value):
    return (value or "").replace("\r", "").strip()

def normalize_header(value):
    return re.sub(r"\s+", " ", clean(value).lower())

def normalize_run(value):
    value = clean(value).replace("_", "-")
    return re.sub(r"/+$", "", value)

def parse_bool(value):
    value = clean(value)
    if not value:
        return None
    lowered = value.lower()
    if lowered in {"1", "true", "t", "yes", "y", "on"}:
        return True
    if lowered in {"0", "false", "f", "no", "n", "off"}:
        return False
    return value

def parse_number(value):
    value = clean(value)
    if not value:
        return None
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value

def parse_list(value):
    return [parse_number(part) for part in re.split(r"[,\s]+", clean(value)) if part]

fields = {
    "run": {
        "aliases": ("date", "run"),
        "parser": normalize_run,
    },
    "spills": {
        "aliases": ("spills",),
        "parser": parse_number,
    },
    "laser_on": {
        "aliases": ("laser on?", "laser on"),
        "parser": parse_bool,
    },
    "laser_intensity": {
        "aliases": ("laser intensity", "intensity"),
        "parser": parse_number,
    },
    "laser_rate_khz": {
        "aliases": ("laser rate (khz)", "laser rate", "rate"),
        "parser": parse_number,
    },
    "radio_src": {
        "aliases": ("radio. src?", "radio src?", "radio src", "radio source"),
        "parser": parse_bool,
    },
    "src_position": {
        "aliases": ("src position", "source position"),
        "parser": clean,
    },
    "vbias": {
        "aliases": ("vbias", "v bias"),
        "parser": parse_number,
    },
    "channels": {
        "aliases": ("channels",),
        "parser": parse_list,
    },
    "operating_mode": {
        "aliases": ("operating mode", "mode"),
        "parser": parse_list,
    },
    "threshold": {
        "aliases": ("threshold",),
        "parser": parse_list,
    },
    "offset1": {
        "aliases": ("offset1",),
        "parser": parse_list,
    },
    "gain1": {
        "aliases": ("gain1",),
        "parser": parse_list,
    },
    "gain2": {
        "aliases": ("gain2",),
        "parser": parse_list,
    },
    "notes": {
        "aliases": ("notes",),
        "parser": clean,
    },
}

with open(csv_path, newline="", encoding="utf-8-sig") as handle:
    reader = csv.reader(handle)
    try:
        header = next(reader)
    except StopIteration:
        header = []
        rows = []
    else:
        rows = list(reader)

header_index = {normalize_header(name): i for i, name in enumerate(header) if clean(name)}
used_columns = set()

def column_for(field):
    for alias in fields[field]["aliases"]:
        if alias in header_index:
            return header_index[alias]
    return None

columns = {field: column_for(field) for field in fields}
for col in columns.values():
    if col is not None:
        used_columns.add(col)

run_order = []
runs = {}

for row_number, row in enumerate(rows, start=2):
    run_col = columns.get("run")
    if run_col is None or run_col >= len(row):
        continue

    raw_run = clean(row[run_col])
    run = normalize_run(raw_run)
    if not run:
        continue

    entry = {
        "run": run,
        "raw_run": raw_run,
        "row": row_number,
    }
    for field, spec in fields.items():
        if field == "run":
            continue
        col = columns.get(field)
        if col is None or col >= len(row):
            value = None
        else:
            value = spec["parser"](row[col])
        entry[field] = value

    extra = {}
    for col, name in enumerate(header):
        key = clean(name)
        if not key or col in used_columns or col >= len(row):
            continue
        value = clean(row[col])
        if value:
            extra[key] = value
    if extra:
        entry["extra"] = extra

    if run not in runs:
        run_order.append(run)
    else:
        print(f"warning: duplicate run in logbook JSON, keeping last row: {run}", file=sys.stderr)
    runs[run] = entry

payload = {
    "schema": "alcor-logbook-v1",
    "source_csv": os.path.basename(csv_path),
    "run_order": run_order,
    "runs": runs,
}

with open(json_path, "w", encoding="utf-8") as handle:
    json.dump(payload, handle, indent=2)
    handle.write("\n")
PY
  echo "Updated logbook JSON: ${json_output}"
fi
