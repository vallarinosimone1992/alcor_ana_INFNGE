#!/usr/bin/env bash

root_tool_path() {
  local qa_dir="$1"
  local tool_name="$2"
  printf '%s/output/.root_tools/%s\n' "${qa_dir}" "${tool_name}"
}

root_tool_build() {
  local qa_dir="$1"
  local tool_name="$2"
  local source="$3"
  local exe
  exe="$(root_tool_path "${qa_dir}" "${tool_name}")"
  local stamp="${exe}.stamp"

  if [ ! -f "${source}" ]; then
    echo "ROOT helper source not found: ${source}" >&2
    return 1
  fi
  if ! command -v root-config >/dev/null 2>&1; then
    echo "root-config not found in PATH" >&2
    return 1
  fi

  local cxx
  cxx="${CXX:-$(root-config --cxx)}"
  local root_signature
  root_signature="$(
    {
      printf 'cxx=%s\n' "${cxx}"
      printf 'version=%s\n' "$(root-config --version)"
      printf 'cflags=%s\n' "$(root-config --cflags)"
      printf 'libs=%s\n' "$(root-config --libs)"
    }
  )"

  local rebuild=0
  if [ ! -x "${exe}" ]; then
    rebuild=1
  elif [ ! -f "${stamp}" ] || [ "$(cat "${stamp}")" != "${root_signature}" ]; then
    rebuild=1
  else
    local newer=""
    newer="$(find "${qa_dir}/macro" -maxdepth 1 \( -name '*.cxx' -o -name '*.h' \) -newer "${exe}" -print -quit 2>/dev/null || true)"
    if [ -n "${newer}" ]; then
      rebuild=1
    fi
  fi

  if [ "${rebuild}" -eq 1 ]; then
    mkdir -p "$(dirname "${exe}")"
    local -a cxxflags
    local -a libs
    read -r -a cxxflags <<< "$(root-config --cflags)"
    read -r -a libs <<< "$(root-config --libs)"
    echo "== Building ROOT helper: ${exe}" >&2
    if ! "${cxx}" "${cxxflags[@]}" "-I${qa_dir}/macro" -O2 "${source}" "${libs[@]}" -o "${exe}"; then
      rm -f "${exe}"
      rm -f "${stamp}"
      return 1
    fi
    printf '%s\n' "${root_signature}" > "${stamp}"
  fi

  printf '%s\n' "${exe}"
}
