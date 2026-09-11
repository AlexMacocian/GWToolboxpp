#!/usr/bin/env bash
# Provision the Linux cross-toolchain: MSVC/Windows SDK headers+libs via xwin,
# plus vkd3d-compiler as the fxc stand-in for the SM3 shaders (see tools/hlsl_compile.py).
#
# Everything lands under $TOOLCHAIN_ROOT (default ./.xwin-toolchain, gitignored). Re-running
# is cheap: each step is skipped when its output already exists.
#
# Needs on PATH beforehand: clang-cl, lld-link, llvm-rc, llvm-lib, llvm-mt, cmake >= 3.29,
# ninja, python3, curl, tar, make, pkg-config, flex, bison, autoreconf, widl, Perl JSON and Vulkan headers.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)/.xwin-toolchain}
XWIN_VERSION=${XWIN_VERSION:-0.10.0}
VKD3D_REVISION=${VKD3D_REVISION:-be62407e706ca155a36e7f7dd4422b479bca32a9}
VKD3D_DIRECTORY="vkd3d-${VKD3D_REVISION:0:8}"

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
mkdir -p "$TOOLCHAIN_ROOT"
cd "$TOOLCHAIN_ROOT"
export TMPDIR="$PWD"

if [ ! -d xwin-sdk ]; then
    echo "==> downloading xwin ${XWIN_VERSION}"
    curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/${XWIN_VERSION}/xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
        | tar -xz
    # --include-debug-libs: without msvcrtd/libcmtd even CMake's own compiler check fails,
    # since its default Debug config links /MDd.
    echo "==> splatting the MSVC + Windows SDK (x86, ~800MB)"
    "./xwin-${XWIN_VERSION}-x86_64-unknown-linux-musl/xwin" \
        --accept-license --arch x86 splat --include-debug-libs --output ./xwin-sdk
fi

# The SDK ships each header under its canonical casing plus an all-lowercase alias, but the
# codebase (and vendored GWCA) spell some of them a third way -- <Shlobj.h>, <ShellApi.h>,
# <DelayImp.h>. Windows resolves those case-insensitively; a native Linux clang does not.
# Link every spelling the sources actually use, skipping any name that already resolves.
#
# The source tree is absent when this runs from the Dockerfile (the image is built before any
# repo is mounted), so scan only the directories that exist and let the container entrypoint
# redo this against the real mount.
SOURCE_DIRS=()
for candidate in Core GWToolboxdll GWToolbox RestClient plugins Dependencies; do
    [ -d "$REPO_ROOT/$candidate" ] && SOURCE_DIRS+=("$REPO_ROOT/$candidate")
done

if [ ${#SOURCE_DIRS[@]} -eq 0 ]; then
    echo "==> no source tree under ${REPO_ROOT}; skipping the header-casing pass for now"
    INCLUDED_HEADERS=""
else
    echo "==> linking SDK headers for the casings the sources use"
    # `|| true`: grep exits non-zero when nothing matches, which pipefail would turn fatal.
    INCLUDED_HEADERS=$(grep -rhoP '(?<=#include <)[A-Za-z0-9_./\\-]+(?=>)' \
        --include=*.cpp --include=*.h --include=*.hpp --include=*.inl \
        "${SOURCE_DIRS[@]}" 2>/dev/null | sort -u || true)
fi

INCLUDE_DIRS=(crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
printf '%s\n' "$INCLUDED_HEADERS" \
    | while read -r included; do
        [ -n "$included" ] || continue
        relative=${included//\\//}
        for directory in "${INCLUDE_DIRS[@]}"; do
            [ -e "xwin-sdk/$directory/$relative" ] && continue 2
        done
        basename=$(basename "$relative")
        actual=$(find xwin-sdk/crt/include xwin-sdk/sdk/include -maxdepth 2 -iname "$basename" 2>/dev/null | head -1)
        [ -n "$actual" ] || continue
        # Never link a name onto itself: that replaces the real header with a symlink loop.
        [ "$(basename "$actual")" = "$basename" ] && continue
        ln -sfn "$(basename "$actual")" "$(dirname "$actual")/$basename"
    done

VKD3D_PATCH="${SCRIPT_DIR}/vkd3d-d3dcompile.patch"
if [ ! -x "${VKD3D_DIRECTORY}/vkd3d-compiler" ] || [ "$VKD3D_PATCH" -nt "${VKD3D_DIRECTORY}/vkd3d-compiler" ]; then
    echo "==> building native vkd3d-compiler ${VKD3D_REVISION} with D3DCompile defaults"
    if [ ! -d "$VKD3D_DIRECTORY" ]; then
        git init --quiet "$VKD3D_DIRECTORY"
        git -C "$VKD3D_DIRECTORY" remote add origin https://gitlab.winehq.org/wine/vkd3d.git
        git -C "$VKD3D_DIRECTORY" fetch --quiet --depth 1 origin "$VKD3D_REVISION"
        git -C "$VKD3D_DIRECTORY" checkout --quiet --detach FETCH_HEAD
    fi
    [ -d SPIRV-Headers ] || git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git SPIRV-Headers
    (
        cd "$VKD3D_DIRECTORY"
        [ -f configure ] || ./autogen.sh
        [ -f Makefile ] || ./configure --prefix="$PWD/inst" --disable-tests --disable-demos \
            CPPFLAGS="${CPPFLAGS:-} -I$PWD/../SPIRV-Headers/include" >/dev/null
        make -f Makefile -f - -j"${JOBS:-$(nproc)}" shader-generated <<'MAKE'
.PHONY: shader-generated
shader-generated: $(BUILT_SOURCES)
MAKE
        if ! git apply --reverse --check "$VKD3D_PATCH" 2>/dev/null; then
            git apply "$VKD3D_PATCH"
        fi
        make -j"${JOBS:-$(nproc)}" vkd3d-compiler
    )
fi
ln -sfn "${VKD3D_DIRECTORY}/vkd3d-compiler" vkd3d-compiler

cat <<EOF

Toolchain ready. Configure and build with:

    export XWIN_SDK="${TOOLCHAIN_ROOT}/xwin-sdk"
    export VKD3D_COMPILER="${TOOLCHAIN_ROOT}/vkd3d-compiler"
    cmake --preset xwin
    cmake --build build-xwin -j\$(nproc)
EOF
