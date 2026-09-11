#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$PWD}"
cd "$REPO_ROOT"

CONFIG=${CONFIG:-RelWithDebInfo}
TARGET=${TARGET:-GWToolboxdll}
JOBS=${JOBS:-$(nproc)}

if [ ! -f "${VCPKG_ROOT:-}/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "Set VCPKG_ROOT to a bootstrapped Linux vcpkg checkout." >&2
    exit 1
fi

export TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-${REPO_ROOT}/.xwin-toolchain}"
mkdir -p "$TOOLCHAIN_ROOT"
export TMPDIR="$TOOLCHAIN_ROOT"
export JOBS
./scripts/xwin/setup-toolchain.sh
export XWIN_SDK="${XWIN_SDK:-${TOOLCHAIN_ROOT}/xwin-sdk}"
export VKD3D_COMPILER="${VKD3D_COMPILER:-${TOOLCHAIN_ROOT}/vkd3d-compiler}"

if [ -n "${GWCA_SOURCE:-}" ]; then
    bash "$GWCA_SOURCE/scripts/build-xwin.sh" --sdk "$XWIN_SDK" \
        --config RelWithDebInfo --jobs "$JOBS" --toolbox "$REPO_ROOT"
fi

fix_ownership() {
    if [ -n "${HOST_UID:-}" ] && [ -n "${HOST_GID:-}" ]; then
        chown -R "$HOST_UID:$HOST_GID" build-xwin bin 2>/dev/null || true
        find GWToolboxdll/Widgets/Minimap/Shaders -maxdepth 1 -type f \
            \( -name '*_vs.h' -o -name '*_ps.h' \) \
            -exec chown "$HOST_UID:$HOST_GID" {} + 2>/dev/null || true
    fi
}
trap fix_ownership EXIT

cmake --preset xwin -DCMAKE_BUILD_TYPE="$CONFIG" "-DFXC:FILEPATH=${FXC:-}" \
    "-DVKD3D_COMPILER:FILEPATH=$VKD3D_COMPILER" \
    -DGWTOOLBOX_ALLOW_UNSUPPORTED_CRASH_DUMPS=ON "$@"

if [ "$TARGET" = "all" ]; then
    cmake --build build-xwin --config "$CONFIG" -j "$JOBS"
else
    cmake --build build-xwin --config "$CONFIG" --target "$TARGET" -j "$JOBS"
fi
