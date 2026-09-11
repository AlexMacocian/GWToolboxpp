#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
IMAGE_NAME="gwtoolboxpp-xwin"

CONFIG="RelWithDebInfo"
TARGET="GWToolboxdll"
CMAKE_ARGS=()
JOBS=""
REBUILD_IMAGE=0
SHELL_ONLY=0
HOST_BUILD=0
GWCA_SOURCE=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --host                                   Compile C++ directly on Linux without Docker
  --gwca-source <path>                      Build and stage a local GWCA checkout (requires --host)
  --config <Debug|RelWithDebInfo|Release>   CMake config to build (default: ${CONFIG})
  --target <name>                           CMake build target (default: ${TARGET}; use "all" for everything)
  --cmake-arg <value>                       Extra argument to pass through to CMake configure
  --jobs <n>                                Parallel build jobs (default: all cores)
  --rebuild-image                           Force a clean rebuild of the docker image
  --shell                                   Drop into an interactive shell in the container instead of building
  -h, --help                                Show this help

Host builds require VCPKG_ROOT and the tools listed by scripts/xwin/setup-toolchain.sh.
The SDK and native SM3 shader compiler are provisioned under .xwin-toolchain.
Output: bin/GWToolboxdll.dll (PE32, Intel 80386).
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST_BUILD=1; shift ;;
        --gwca-source) GWCA_SOURCE="$2"; shift 2 ;;
        --config) CONFIG="$2"; shift 2 ;;
        --target) TARGET="$2"; shift 2 ;;
        --cmake-arg) CMAKE_ARGS+=("$2"); shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --rebuild-image) REBUILD_IMAGE=1; shift ;;
        --shell) SHELL_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
    esac
done

case "$CONFIG" in
    Debug|RelWithDebInfo|Release) ;;
    *) echo "Unsupported configuration: $CONFIG" >&2; exit 1 ;;
esac
if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer." >&2
    exit 1
fi
if [ -n "$GWCA_SOURCE" ]; then
    [ "$HOST_BUILD" -eq 1 ] || { echo "--gwca-source requires --host." >&2; exit 1; }
    GWCA_SOURCE=$(cd "$GWCA_SOURCE" && pwd)
    [ -f "$GWCA_SOURCE/scripts/build-xwin.sh" ] || { echo "GWCA checkout has no xwin build script." >&2; exit 1; }
    export GWCA_SOURCE
fi

if [ "$HOST_BUILD" -eq 1 ]; then
    if [ "$REBUILD_IMAGE" -eq 1 ] || [ "$SHELL_ONLY" -eq 1 ]; then
        echo "--host cannot be combined with --rebuild-image or --shell." >&2
        exit 1
    fi
    cd "$REPO_ROOT"
    export CONFIG TARGET JOBS
    export TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-${REPO_ROOT}/.xwin-toolchain}"
    exec "${SCRIPT_DIR}/xwin/build-in-container.sh" "${CMAKE_ARGS[@]}"
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "docker is required; use --host to build directly on Linux." >&2
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "Docker is not running or is inaccessible; use --host to build directly on Linux." >&2
    exit 1
fi

if [ "${REBUILD_IMAGE}" -eq 1 ] || ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo "[build-xwin] building docker image '${IMAGE_NAME}' (this downloads the MSVC + Windows SDK headers/libs and builds vkd3d; expect it to take a while the first time)..."
    BUILD_ARGS=()
    [ "$REBUILD_IMAGE" -eq 0 ] || BUILD_ARGS+=(--no-cache)
    docker build "${BUILD_ARGS[@]}" -t "${IMAGE_NAME}" -f "${SCRIPT_DIR}/xwin/Dockerfile" "${SCRIPT_DIR}/xwin"
fi

TTY_FLAGS=()
if [ -t 0 ] && [ -t 1 ]; then
    TTY_FLAGS=(-it)
fi

RUN_ARGS=(--rm "${TTY_FLAGS[@]}" -v "${REPO_ROOT}:/src" -w /src -e CONFIG="${CONFIG}" -e TARGET="${TARGET}" -e JOBS="${JOBS}" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" "${IMAGE_NAME}")

if [ "${SHELL_ONLY}" -eq 1 ]; then
    exec docker run "${RUN_ARGS[@]}" bash
fi

echo "[build-xwin] configuring (preset: xwin, config: ${CONFIG})..."
exec docker run "${RUN_ARGS[@]}" /src/scripts/xwin/build-in-container.sh "${CMAKE_ARGS[@]}"
