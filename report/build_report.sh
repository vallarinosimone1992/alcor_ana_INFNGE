#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}"

tex_file="report.tex"
log_dir="${script_dir}/.build"
mkdir -p "${log_dir}"

if ! command -v pdflatex >/dev/null 2>&1; then
  echo "pdflatex not found. Please install a LaTeX distribution (e.g. TeX Live or MacTeX)." >&2
  exit 1
fi

if [[ ! -f "${tex_file}" ]]; then
  echo "Missing ${tex_file} in ${script_dir}" >&2
  exit 1
fi

run_pdflatex() {
  local pass="$1"
  local log="${log_dir}/pdflatex_pass${pass}.log"
  echo "Running pdflatex (pass ${pass})..."
  pdflatex -interaction=nonstopmode -halt-on-error "${tex_file}" >"${log}" 2>&1
  local rc=$?
  if [[ ${rc} -ne 0 ]]; then
    echo "pdflatex failed on pass ${pass} (exit ${rc})." >&2
    echo "---- Errors (lines starting with '!') ----" >&2
    grep -n '^!' "${log}" >&2 || true
    echo "---- Last 40 lines of log ----" >&2
    tail -n 40 "${log}" >&2 || true
    echo "Full log: ${log}" >&2
    exit "${rc}"
  fi
}

run_pdflatex 1
run_pdflatex 2

echo "Built report.pdf"
