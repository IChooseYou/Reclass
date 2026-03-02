#!/usr/bin/env bash
set -euo pipefail

print_help() {
  cat <<'EOF'
Reclass macOS Build Script

Usage:
  ./scripts/build_macos.sh [options]

Options:
  --qt-dir <path>       Qt installation prefix (e.g. /opt/homebrew/opt/qt)
  --build-type <type>   Release | Debug | RelWithDebInfo | MinSizeRel (default: Release)
  --build-dir <path>    Build directory (default: <repo>/build)
  --generator <name>    CMake generator (default: Ninja if available)
  --clean               Remove build directory before configuring
  --rebuild             Clean then build
  --package             Run macdeployqt and create a zip
  --tests               Run ctest after build
  -h, --help            Show this help

Notes:
  - You can set QTDIR or Qt6_DIR in your environment instead of --qt-dir.
  - If Qt is installed via Homebrew, the script will try to detect it.
EOF
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/.." && pwd)"

qt_dir=""
build_type="Release"
build_dir="${project_root}/build"
generator=""
do_clean="false"
do_package="false"
do_tests="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --qt-dir)
      qt_dir="${2:-}"
      shift 2
      ;;
    --build-type)
      build_type="${2:-}"
      shift 2
      ;;
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --generator)
      generator="${2:-}"
      shift 2
      ;;
    --clean)
      do_clean="true"
      shift
      ;;
    --rebuild)
      do_clean="true"
      shift
      ;;
    --package)
      do_package="true"
      shift
      ;;
    --tests)
      do_tests="true"
      shift
      ;;
    -h|--help)
      print_help
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      print_help
      exit 1
      ;;
  esac
done

if [[ -z "${qt_dir}" ]]; then
  if [[ -n "${QTDIR:-}" ]]; then
    qt_dir="${QTDIR}"
  elif [[ -n "${Qt6_DIR:-}" ]]; then
    qt_dir="${Qt6_DIR}"
  elif command -v brew >/dev/null 2>&1; then
    if brew --prefix qt >/dev/null 2>&1; then
      qt_dir="$(brew --prefix qt)"
    fi
  fi
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake not found. Install CMake and try again." >&2
  exit 1
fi

if [[ -z "${generator}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
  fi
fi

if [[ "${do_clean}" == "true" && -d "${build_dir}" ]]; then
  echo "Cleaning build directory: ${build_dir}"
  rm -rf "${build_dir}"
fi

mkdir -p "${build_dir}"

cmake_args=(
  -S "${project_root}"
  -B "${build_dir}"
  -DCMAKE_BUILD_TYPE="${build_type}"
)

if [[ -n "${generator}" ]]; then
  cmake_args+=(-G "${generator}")
fi

if [[ -n "${qt_dir}" ]]; then
  export PATH="${qt_dir}/bin:${PATH}"
  cmake_args+=(-DCMAKE_PREFIX_PATH="${qt_dir}")
fi

echo "Configuring..."
cmake "${cmake_args[@]}"

echo "Building..."
cmake --build "${build_dir}" --config "${build_type}"

if [[ "${do_tests}" == "true" ]]; then
  echo "Running tests..."
  ctest --test-dir "${build_dir}" --output-on-failure -C "${build_type}"
fi

if [[ "${do_package}" == "true" ]]; then
  app_path="${build_dir}/Reclass.app"
  if [[ ! -d "${app_path}" ]]; then
    echo "ERROR: ${app_path} not found. Build may have failed." >&2
    exit 1
  fi

  macdeployqt_bin=""
  if [[ -n "${qt_dir}" && -x "${qt_dir}/bin/macdeployqt" ]]; then
    macdeployqt_bin="${qt_dir}/bin/macdeployqt"
  elif command -v macdeployqt >/dev/null 2>&1; then
    macdeployqt_bin="$(command -v macdeployqt)"
  fi

  if [[ -z "${macdeployqt_bin}" ]]; then
    echo "ERROR: macdeployqt not found. Ensure Qt is installed and in PATH." >&2
    exit 1
  fi

  echo "Running macdeployqt..."
  "${macdeployqt_bin}" "${app_path}" -always-overwrite

  arch="$(uname -m)"
  zip_name="Reclass-macos-${arch}-qt6.zip"
  echo "Creating zip: ${zip_name}"
  ditto -c -k --sequesterRsrc --keepParent "${app_path}" "${build_dir}/${zip_name}"
  echo "Packaged: ${build_dir}/${zip_name}"
fi
