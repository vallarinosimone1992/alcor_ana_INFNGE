#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: build_decoder.sh [options]

Builds the ALCOR decoder from the local source tree (decoder/src).

Optional options:
  -s, --src DIR           source dir (default: ../decoder/src)
  -b, --build DIR         build dir (default: ../decoder/build)
  -i, --install DIR       install dir (default: ../decoder/local)
  -j, --jobs N            parallel jobs (default: auto)
      --clean             remove build dir before configuring
  -h, --help              show this help
USAGE
}

src_dir=""
build_dir=""
install_dir=""
bin_dir=""
jobs=""
clean=0

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
    -s|--src)
      need_arg "$@"
      src_dir=${2:-}
      shift 2
      ;;
    --src=*)
      src_dir=${1#*=}
      shift
      ;;
    -b|--build)
      need_arg "$@"
      build_dir=${2:-}
      shift 2
      ;;
    --build=*)
      build_dir=${1#*=}
      shift
      ;;
    -i|--install)
      need_arg "$@"
      install_dir=${2:-}
      shift 2
      ;;
    --install=*)
      install_dir=${1#*=}
      shift
      ;;
    -j|--jobs)
      need_arg "$@"
      jobs=${2:-}
      shift 2
      ;;
    --jobs=*)
      jobs=${1#*=}
      shift
      ;;
    --clean)
      clean=1
      shift
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      echo "Unexpected argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
qa_dir="${ALCOR_ANA_GE:-$(cd "${script_dir}/.." && pwd)}"

if [ -z "${src_dir}" ]; then
  src_dir="${qa_dir}/decoder/src"
fi
if [ -z "${build_dir}" ]; then
  build_dir="${qa_dir}/decoder/build"
fi
if [ -z "${install_dir}" ]; then
  install_dir="${qa_dir}/decoder/local"
fi
if [ -z "${bin_dir}" ]; then
  bin_dir="${qa_dir}/decoder/bin"
fi

if [ ! -d "${src_dir}" ]; then
  echo "decoder source dir not found: ${src_dir}" >&2
  exit 1
fi

if [ "${clean}" -eq 1 ] && [ -d "${build_dir}" ]; then
  echo "Removing build dir: ${build_dir}"
  rm -rf "${build_dir}"
fi

if [ -z "${jobs}" ]; then
  if command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
  else
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
  fi
  if [ -z "${jobs}" ]; then
    jobs=4
  fi
fi

mkdir -p "${build_dir}" "${install_dir}" "${bin_dir}"

echo "Source: ${src_dir}"
echo "Build: ${build_dir}"
echo "Install: ${install_dir}"
echo "Jobs: ${jobs}"

cmake -S "${src_dir}" -B "${build_dir}" -DCMAKE_INSTALL_PREFIX="${install_dir}" -DALCOR_BUILD_READOUT=OFF
cmake --build "${build_dir}" --target decoder -j "${jobs}"
cmake --install "${build_dir}"

decoder_target=""
if [ -x "${build_dir}/src/decoder" ]; then
  decoder_target="${build_dir}/src/decoder"
elif [ -x "${install_dir}/bin/decoder" ]; then
  decoder_target="${install_dir}/bin/decoder"
elif [ -x "${install_dir}/decoder" ]; then
  decoder_target="${install_dir}/decoder"
elif [ -x "${src_dir}/bin/decoder" ]; then
  decoder_target="${src_dir}/bin/decoder"
fi

if [ -n "${decoder_target}" ]; then
  root_lib_dir="$(root-config --libdir 2>/dev/null || true)"
  cat > "${bin_dir}/decoder" <<EOF
#!/usr/bin/env bash
set -euo pipefail
decoder_target="${decoder_target}"
root_lib_dir="${root_lib_dir}"
if [ -n "\${root_lib_dir}" ]; then
  export LD_LIBRARY_PATH="\${root_lib_dir}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
fi
exec "\${decoder_target}" "\$@"
EOF
  chmod +x "${bin_dir}/decoder"
fi

if [ -x "${bin_dir}/decoder" ]; then
  echo "Decoder built at: ${bin_dir}/decoder"
  if [ -n "${root_lib_dir:-}" ]; then
    echo "Decoder runtime ROOT libdir: ${root_lib_dir}"
  fi
else
  echo "Decoder build completed, but decoder binary not found in ${bin_dir}" >&2
  exit 1
fi
