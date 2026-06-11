#!/usr/bin/env bash

inputs_has_root_files() {
  local dir="$1"
  compgen -G "${dir}/alcdaq.fifo_*.root" >/dev/null
}

inputs_resolve_one() {
  local input="$1"
  if [ -f "${input}" ]; then
    if [[ "${input}" == *.root ]]; then
      printf '%s\n' "${input}"
      return 0
    fi
    echo "input file is not a ROOT file: ${input}" >&2
    return 1
  fi

  if [ ! -d "${input}" ]; then
    echo "input path not found: ${input}" >&2
    return 1
  fi

  if inputs_has_root_files "${input}"; then
    find -L "${input}" -maxdepth 1 -type f -name 'alcdaq.fifo_*.root' | sort
    return 0
  fi

  if [ -d "${input}/decoded" ] && inputs_has_root_files "${input}/decoded"; then
    find -L "${input}/decoded" -maxdepth 1 -type f -name 'alcdaq.fifo_*.root' | sort
    return 0
  fi

  local matches=()
  while IFS= read -r cand; do
    if inputs_has_root_files "${cand}"; then
      matches+=("${cand}")
    fi
  done < <(find -L "${input}" -maxdepth 3 -type d -name decoded 2>/dev/null | sort)

  if [ "${#matches[@]}" -eq 1 ]; then
    find -L "${matches[0]}" -maxdepth 1 -type f -name 'alcdaq.fifo_*.root' | sort
    return 0
  fi

  if [ "${#matches[@]}" -gt 1 ]; then
    echo "multiple decoded directories found under ${input}:" >&2
    printf '  %s\n' "${matches[@]}" >&2
    return 1
  fi

  echo "no decoded ROOT files found under ${input}" >&2
  return 1
}

inputs_write_root_list() {
  local list_file="$1"
  shift
  : > "${list_file}"
  local input
  for input in "$@"; do
    inputs_resolve_one "${input}" >> "${list_file}"
  done
  if [ ! -s "${list_file}" ]; then
    echo "no ROOT files resolved into ${list_file}" >&2
    return 1
  fi
}

inputs_first_resolved_path() {
  local input="$1"
  inputs_resolve_one "${input}" | head -n 1
}
