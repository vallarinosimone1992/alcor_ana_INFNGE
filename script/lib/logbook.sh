#!/usr/bin/env bash

logbook_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
logbook_qa_dir="${ALCOR_ANA_GE:-$(cd "${logbook_script_dir}/../.." && pwd)}"

logbook_default_path() {
  printf '%s\n' "${logbook_qa_dir}/config/logbook.csv"
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
  awk -F, -v target="${run}" '
    function trim(s) {
      gsub(/\r/, "", s)
      gsub(/^[ \t]+|[ \t]+$/, "", s)
      return s
    }
    function norm(s) {
      s = trim(s)
      gsub(/_/, "-", s)
      sub(/\/+$/, "", s)
      return s
    }
    NR == 1 { next }
    norm($1) == target {
      for (i = 1; i <= 11; ++i) {
        if (i > 1) {
          printf "\t"
        }
        printf "%s", trim($i)
      }
      printf "\n"
      found = 1
      exit
    }
    END { if (!found) exit 1 }
  ' "${logbook}"
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
  logbook_channels "${logbook}" "${run}" | awk '{
    out = ""
    for (i = 1; i <= NF; ++i) {
      if (out != "") out = out ","
      out = out $i
    }
    print out
  }'
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
      n = split(modes, values, /[[:space:]]+/)
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
  awk -F, -v required="${required}" '
    function trim(s) {
      gsub(/\r/, "", s)
      gsub(/^[ \t]+|[ \t]+$/, "", s)
      return s
    }
    function norm(s) {
      s = trim(s)
      gsub(/_/, "-", s)
      sub(/\/+$/, "", s)
      return s
    }
    NR == 1 { next }
    {
      mode = trim($7)
      n = split(mode, values, /[[:space:]]+/)
      ok = 1
      seen = 0
      for (i = 1; i <= n; ++i) {
        if (values[i] == "") continue
        seen = 1
        if (values[i] != required) ok = 0
      }
      if (seen && ok) print norm($1)
    }
  ' "${logbook}"
}
