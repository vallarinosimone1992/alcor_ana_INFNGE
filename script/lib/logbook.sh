#!/usr/bin/env bash

logbook_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
logbook_qa_dir="${ALCOR_ANA_GE:-$(cd "${logbook_script_dir}/../.." && pwd)}"

logbook_default_path() {
  printf '%s\n' "${logbook_qa_dir}/config/logbook.json"
}

logbook_trim() {
  local value="${1//$'\r'/}"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s\n' "${value}"
}

logbook_normalize_run() {
  local run
  run="$(logbook_trim "$1")"
  run="${run//_/-}"
  while [[ "${run}" == */ ]]; do
    run="${run%/}"
  done
  printf '%s\n' "${run}"
}

logbook_run_from_path() {
  local path="${1%/}"
  local token=""
  IFS='/' read -r -a parts <<< "${path}"
  for part in "${parts[@]}"; do
    part="${part%/}"
    if [[ "${part}" =~ ^[0-9]{8}[-_][0-9]{6}$ ]]; then
      token="${part}"
    fi
  done
  if [ -z "${token}" ]; then
    token="$(basename "${path}")"
  fi
  logbook_normalize_run "${token}"
}

logbook_awk_row() {
  local logbook="$1"
  local run
  run="$(logbook_normalize_run "$2")"
  python3 - "${logbook}" "${run}" <<'PY'
import csv
import json
import re
import sys

logbook, target = sys.argv[1], sys.argv[2]

def clean(value):
    return (value or "").replace("\r", "").strip()

def norm_run(value):
    value = clean(value).replace("_", "-")
    return re.sub(r"/+$", "", value)

def norm_header(value):
    return re.sub(r"\s+", " ", clean(value).lower())

def stringify(value):
    if value is None:
        return ""
    if isinstance(value, list):
        return " ".join(stringify(part) for part in value)
    return str(value)

fields = (
    "date",
    "spills",
    "intensity",
    "rate",
    "vbias",
    "channels",
    "operating_mode",
    "threshold",
    "offset1",
    "gain1",
    "gain2",
)

json_keys = {
    "date": ("run", "date"),
    "spills": ("spills",),
    "intensity": ("laser_intensity", "intensity"),
    "rate": ("laser_rate_khz", "laser_rate", "rate"),
    "vbias": ("vbias",),
    "channels": ("channels",),
    "operating_mode": ("operating_mode", "mode"),
    "threshold": ("threshold",),
    "offset1": ("offset1",),
    "gain1": ("gain1",),
    "gain2": ("gain2",),
}

aliases = {
    "date": ("date", "run"),
    "spills": ("spills",),
    "intensity": ("laser intensity", "intensity"),
    "rate": ("laser rate (khz)", "laser rate", "rate"),
    "vbias": ("vbias", "v bias"),
    "channels": ("channels",),
    "operating_mode": ("operating mode", "mode"),
    "threshold": ("threshold",),
    "offset1": ("offset1",),
    "gain1": ("gain1",),
    "gain2": ("gain2",),
}

def looks_like_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        for chunk in iter(lambda: handle.read(4096), ""):
            stripped = chunk.lstrip()
            if stripped:
                return stripped[0] == "{"
    return False

if looks_like_json(logbook):
    with open(logbook, "r", encoding="utf-8-sig") as handle:
        payload = json.load(handle)

    runs = payload.get("runs", {})
    entry = runs.get(target)
    if entry is None:
        for key, candidate in runs.items():
            if norm_run(key) == target or norm_run(candidate.get("run", "")) == target or norm_run(candidate.get("raw_run", "")) == target:
                entry = candidate
                break
    if entry is None:
        sys.exit(1)

    out = []
    for field in fields:
        value = None
        for key in json_keys[field]:
            if key in entry:
                value = entry[key]
                break
        out.append(stringify(value))
    print("\t".join(out))
    sys.exit(0)

with open(logbook, newline="", encoding="utf-8-sig") as handle:
    reader = csv.reader(handle)
    try:
        header = next(reader)
    except StopIteration:
        sys.exit(1)

    index = {norm_header(name): i for i, name in enumerate(header) if clean(name)}

    def column(field):
        for alias in aliases[field]:
            if alias in index:
                return index[alias]
        return None

    columns = {field: column(field) for field in fields}
    date_col = columns.get("date")
    if date_col is None:
        date_col = 0

    for row in reader:
        if date_col >= len(row) or norm_run(row[date_col]) != target:
            continue
        out = []
        for field in fields:
            col = columns.get(field)
            out.append(clean(row[col]) if col is not None and col < len(row) else "")
        print("\t".join(out))
        sys.exit(0)

sys.exit(1)
PY
}

logbook_has_run() {
  local logbook="$1"
  local run="$2"
  logbook_awk_row "${logbook}" "${run}" >/dev/null
}

logbook_field() {
  local logbook="$1"
  local run="$2"
  local field="$3"
  local index
  case "${field}" in
    date|run) index=1 ;;
    spills) index=2 ;;
    intensity|laser_intensity) index=3 ;;
    rate|laser_rate) index=4 ;;
    vbias) index=5 ;;
    channels) index=6 ;;
    operating_mode|mode) index=7 ;;
    threshold) index=8 ;;
    offset1) index=9 ;;
    gain1) index=10 ;;
    gain2) index=11 ;;
    *)
      echo "unknown logbook field: ${field}" >&2
      return 2
      ;;
  esac
  logbook_awk_row "${logbook}" "${run}" | awk -F'\t' -v i="${index}" '{print $i}'
}

logbook_channels() {
  local logbook="$1"
  local run="$2"
  logbook_field "${logbook}" "${run}" channels
}

logbook_channels_csv() {
  local logbook="$1"
  local run="$2"
  logbook_channels "${logbook}" "${run}" | python3 -c '
import re
import sys
value = sys.stdin.read().strip()
print(",".join(part for part in re.split(r"[,\s]+", value) if part))
'
}

logbook_modes() {
  local logbook="$1"
  local run="$2"
  logbook_field "${logbook}" "${run}" operating_mode
}

logbook_require_mode() {
  local logbook="$1"
  local run="$2"
  local required="$3"
  local modes
  modes="$(logbook_modes "${logbook}" "${run}")" || {
    echo "run not found in logbook: $(logbook_normalize_run "${run}")" >&2
    return 1
  }
  awk -v modes="${modes}" -v required="${required}" '
    BEGIN {
      n = split(modes, values, /[[:space:],]+/)
      if (n == 0) exit 1
      for (i = 1; i <= n; ++i) {
        if (values[i] == "") continue
        if (values[i] != required) {
          exit 2
        }
        seen = 1
      }
      exit seen ? 0 : 1
    }
  ' || {
    echo "run $(logbook_normalize_run "${run}") is not eligible: Operating Mode='${modes}', required='${required}'" >&2
    return 1
  }
}

logbook_list_runs_by_mode() {
  local logbook="$1"
  local required="$2"
  python3 - "${logbook}" "${required}" <<'PY'
import csv
import json
import re
import sys

logbook, required = sys.argv[1], sys.argv[2]

def clean(value):
    return (value or "").replace("\r", "").strip()

def norm_run(value):
    value = clean(value).replace("_", "-")
    return re.sub(r"/+$", "", value)

def norm_header(value):
    return re.sub(r"\s+", " ", clean(value).lower())

def looks_like_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        for chunk in iter(lambda: handle.read(4096), ""):
            stripped = chunk.lstrip()
            if stripped:
                return stripped[0] == "{"
    return False

def split_values(value):
    if value is None:
        return []
    if isinstance(value, list):
        return [str(part) for part in value if str(part) != ""]
    return [part for part in re.split(r"[,\s]+", clean(value)) if part]

if looks_like_json(logbook):
    with open(logbook, "r", encoding="utf-8-sig") as handle:
        payload = json.load(handle)
    runs = payload.get("runs", {})
    order = payload.get("run_order") or list(runs.keys())
    for run in order:
        entry = runs.get(run)
        if not entry:
            continue
        modes = split_values(entry.get("operating_mode"))
        if modes and all(mode == required for mode in modes):
            print(norm_run(entry.get("run", run)))
    sys.exit(0)

with open(logbook, newline="", encoding="utf-8-sig") as handle:
    reader = csv.reader(handle)
    try:
        header = next(reader)
    except StopIteration:
        sys.exit(0)

    index = {norm_header(name): i for i, name in enumerate(header) if clean(name)}
    date_col = index.get("date", index.get("run", 0))
    mode_col = index.get("operating mode", index.get("mode"))
    if mode_col is None:
        sys.exit(0)

    for row in reader:
        if date_col >= len(row) or mode_col >= len(row):
            continue
        modes = [part for part in re.split(r"[,\s]+", clean(row[mode_col])) if part]
        if modes and all(mode == required for mode in modes):
            print(norm_run(row[date_col]))
PY
}

logbook_list_runs() {
  local logbook="$1"
  python3 - "${logbook}" <<'PY'
import csv
import json
import re
import sys

logbook = sys.argv[1]

def clean(value):
    return (value or "").replace("\r", "").strip()

def norm_run(value):
    value = clean(value).replace("_", "-")
    return re.sub(r"/+$", "", value)

def norm_header(value):
    return re.sub(r"\s+", " ", clean(value).lower())

def looks_like_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        for chunk in iter(lambda: handle.read(4096), ""):
            stripped = chunk.lstrip()
            if stripped:
                return stripped[0] == "{"
    return False

if looks_like_json(logbook):
    with open(logbook, "r", encoding="utf-8-sig") as handle:
        payload = json.load(handle)
    runs = payload.get("runs", {})
    order = payload.get("run_order") or list(runs.keys())
    for run in order:
        entry = runs.get(run, {})
        print(norm_run(entry.get("run", run)))
    sys.exit(0)

with open(logbook, newline="", encoding="utf-8-sig") as handle:
    reader = csv.reader(handle)
    try:
        header = next(reader)
    except StopIteration:
        sys.exit(0)

    index = {norm_header(name): i for i, name in enumerate(header) if clean(name)}
    date_col = index.get("date", index.get("run", 0))
    for row in reader:
        if date_col < len(row):
            run = norm_run(row[date_col])
            if run:
                print(run)
PY
}
