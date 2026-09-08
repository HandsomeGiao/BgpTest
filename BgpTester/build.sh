#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build.sh [options] [-- CMAKE_ARGS...]

Build the portable Linux CLI (requires CMake 3.25+ and a C++20 toolchain).

Options:
  -c, --config CONFIG    Release (default), Debug, RelWithDebInfo or MinSizeRel
  -j, --jobs N           Parallel jobs (default: CMAKE_BUILD_PARALLEL_LEVEL or CPU count)
  -G, --generator NAME   CMake generator (default: CMake's selection or cached generator)
  -h, --help             Show this help
  -- CMAKE_ARGS...       Extra configure options, e.g. compiler or dependency paths

Output: <project>/build/<CONFIG>/bin/BgpTesterCli
GUI and tests are disabled. The script does not run the CLI.

Examples:
  ./build.sh
  ./build.sh --config Debug --jobs 2
  ./build.sh --generator Ninja -- -DCMAKE_CXX_COMPILER=clang++
EOF
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 2
}

configuration=Release
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-}"
generator=
cmake_args=()

while (($# > 0)); do
    case "$1" in
        -c|--config|-j|--jobs|-G|--generator)
            if (($# < 2)) || [[ -z "$2" || "$2" == -* ]]; then
                fail "$1 requires a value."
            fi
            case "$1" in
                -c|--config) configuration="$2" ;;
                -j|--jobs) jobs="$2" ;;
                -G|--generator) generator="$2" ;;
            esac
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            cmake_args=("$@")
            break
            ;;
        *)
            fail "Unknown option: $1. Use --help for usage."
            ;;
    esac
done

case "$configuration" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    *) fail "Invalid configuration: $configuration." ;;
esac

if [[ -z "$jobs" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "Parallel jobs must be a positive integer."
command -v cmake >/dev/null 2>&1 || fail "CMake was not found. Install CMake 3.25 or newer and add it to PATH."

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$project_dir/build/$configuration"

# Keep the CLI-only settings explicit even when reusing a CMake cache.
configure_args=(
    "${cmake_args[@]}"
    -S "$project_dir"
    -B "$build_dir"
    "-DCMAKE_BUILD_TYPE=$configuration"
    -DBGPTESTER_BUILD_GUI=OFF
    -DBUILD_TESTING=OFF
)
if [[ -n "$generator" ]]; then
    configure_args+=(-G "$generator")
fi

cmake "${configure_args[@]}"
cmake --build "$build_dir" --config "$configuration" --target BgpTesterCli --parallel "$jobs"

printf '\nCLI built: %s/bin/BgpTesterCli\n' "$build_dir"
