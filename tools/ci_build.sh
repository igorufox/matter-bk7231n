#!/bin/bash
# Native Linux build of libMatterApp.a and its release package (GitHub Actions, or Linux / WSL by hand):
#
#   tools/ci_build.sh <deps_dir> <out_dir>
#
# <deps_dir> gets everything the build needs, at the versions pinned below (skipped when already there, so it can be
# cached): connectedhomeip with the submodules the library uses, a Python venv for the code generators, zap-cli,
# LibreTiny + the BDK packages it uses for the BK7231N (headers only) and the Linux build of the arm-none-eabi gcc
# LibreTiny compiles the firmware with (PlatformIO package, PLATFORMIO_CORE_DIR=<deps_dir>/pio).
# <out_dir> gets the ninja build (build/) and the package:
#   matter-bk7231n-lib.zip         lib/libMatterApp.a (debug info stripped), include/lwipopts.h and the headers of the
#                                  library's API with their flags (include/chip, tools/package.py)
#   matter-bk7231n-lib.zip.sha256  "<sha256>  matter-bk7231n-lib.zip"
#
# Needs git, curl, unzip, python3 with venv and gn on PATH (Ubuntu 24.04: apt install generate-ninja). The Windows
# gcc build from WSL is tools/build.sh; both use the same GN root, this one with bk_toolchain = "native".
set -euo pipefail

M=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1" "$2"
DEPS=$(cd "$1" && pwd)
OUT=$(cd "$2" && pwd)
JOBS=${JOBS:-8}

# connectedhomeip b87051a95f = Matter 1.5, the commit of esp_matter 1.5.1 (the ESP32 build of ESPurna)
CHIP_URL=https://github.com/project-chip/connectedhomeip
CHIP_COMMIT=b87051a95fc110c0cfbac278f232df55c89866de
CHIP_SUBMODULES="third_party/nlassert/repo third_party/nlio/repo third_party/mbedtls/repo third_party/jsoncpp/repo
                 third_party/pigweed/repo"
ZAP_VERSION=v2025.10.23-nightly          # scripts/setup/zap.version of that commit
GCC_PACKAGE=platformio/toolchain-gccarmnoneeabi@1.100301.220327   # GCC 10.3.1 (10.3-2021.10), LibreTiny BK7231N
GCC_VERSION=10.3.1
# LibreTiny 1.14.0 and its BK7231N packages: <path under pio home> <repository> <commit> (tag)
LT_REPOS=(
    "platforms/libretiny            https://github.com/libretiny-eu/libretiny             c81bc9cef83ffd42fcffce32b8b08793b2400b9f" # v1.14.0
    "packages/framework-beken-bdk   https://github.com/libretiny-eu/framework-beken-bdk   841343e64d7584a3a650222ab0b6ea967755352a" # 3.0.33
    "packages/library-freertos      https://github.com/libretiny-eu/library-freertos      dd0b977a86ea18594167c4dfeb775ea98bfe59e4" # 9.0.0
    "packages/library-freertos-port https://github.com/libretiny-eu/library-freertos-port 91a88be92c5b0d8a36922063ca5ed6850193a115" # 2026.02.04
    "packages/library-lwip          https://github.com/libretiny-eu/lwip                  0d1cedf257bf58ac7c12fb7b79ec79daf2a006bd" # 2.2.0-bdk
)

CHIP=$DEPS/connectedhomeip
PIO=$DEPS/pio
VENV=$DEPS/venv
ZAP=$DEPS/zap-$ZAP_VERSION

# shallow checkout of one commit; .ci-commit marks a finished one
checkout() {   # <dir> <url> <commit>
    if [ "$(cat "$1/.ci-commit" 2>/dev/null)" = "$3" ]; then
        return
    fi
    echo "== $2 @ $3"
    rm -rf "$1"
    git init -q "$1"
    git -C "$1" remote add origin "$2"
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
    echo "$3" > "$1/.ci-commit"
}

checkout "$CHIP" "$CHIP_URL" "$CHIP_COMMIT"
if [ ! -f "$CHIP/.ci-submodules" ]; then
    # shellcheck disable=SC2086
    git -C "$CHIP" submodule update -q --init --depth 1 -- $CHIP_SUBMODULES
    touch "$CHIP/.ci-submodules"
fi

for repo in "${LT_REPOS[@]}"; do
    read -r path url commit <<< "$repo"
    checkout "$PIO/$path" "$url" "$commit"
done

if [ ! -f "$VENV/.ci-done" ]; then
    echo "== Python venv"
    rm -rf "$VENV"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$CHIP/scripts/setup/requirements.build.txt" ninja platformio
    touch "$VENV/.ci-done"
fi

GCC_BIN=$PIO/packages/toolchain-gccarmnoneeabi/bin
if [ ! -x "$GCC_BIN/arm-none-eabi-gcc" ]; then
    echo "== $GCC_PACKAGE"
    PLATFORMIO_CORE_DIR=$PIO "$VENV/bin/pio" pkg install -g -t "$GCC_PACKAGE"
fi
"$GCC_BIN/arm-none-eabi-gcc" --version | head -1 | grep -q " $GCC_VERSION " || {
    echo "arm-none-eabi-gcc is not $GCC_VERSION:"; "$GCC_BIN/arm-none-eabi-gcc" --version | head -1; exit 1; }

if [ ! -x "$ZAP/zap-cli" ]; then
    echo "== zap-cli $ZAP_VERSION"
    rm -rf "$ZAP" "$ZAP.zip"
    curl -fsSL -o "$ZAP.zip" "https://github.com/project-chip/zap/releases/download/$ZAP_VERSION/zap-linux-x64.zip"
    unzip -q "$ZAP.zip" -d "$ZAP"
    rm "$ZAP.zip"
    chmod +x "$ZAP/zap-cli"
fi

command -v gn > /dev/null || { echo "gn is not on PATH (Ubuntu: apt install generate-ninja)"; exit 1; }

# the GN root finds connectedhomeip and the PlatformIO packages through these links (tools/build.sh makes NTFS
# junctions instead, for the Windows gcc)
mkdir -p "$M/gn/third_party"
for link in "connectedhomeip:$CHIP" "pio:$PIO"; do
    name=${link%%:*}
    target=${link#*:}
    rm -rf "${M:?}/gn/third_party/$name"
    ln -s "$target" "$M/gn/third_party/$name"
done
# the data model target has to live under connectedhomeip/examples (visibility of src/app:required-privileges)
mkdir -p "$CHIP/examples/bk7231n-plug"
cp "$M/gn/chip-examples/bk7231n-plug/BUILD.gn" "$CHIP/examples/bk7231n-plug/BUILD.gn"

# shellcheck disable=SC1091
. "$VENV/bin/activate"
export PATH=$ZAP:$PATH
export ZAP_INSTALL_PATH=$ZAP
export PYTHONPATH=$CHIP/scripts/py_matter_idl:$CHIP/scripts/py_matter_yamltests

echo "== build: connectedhomeip $(git -C "$CHIP" log -1 --format='%h %ad' --date=short)," \
     "$("$GCC_BIN/arm-none-eabi-gcc" --version | head -1), gn $(gn --version), ninja $(ninja --version)"
cd "$M/gn"
gn gen --check=false "$OUT/build" --args='bk_toolchain="native"
chip_project_config_include="<app/BK7231NProjectConfig.h>"
chip_device_project_config_include="<app/BK7231NProjectConfig.h>"'
ninja -j "$JOBS" -C "$OUT/build" default

echo "== package"
rm -rf "$OUT/pkg"
"$GCC_BIN/arm-none-eabi-strip" --strip-debug -o "$OUT/libMatterApp.stripped.a" "$OUT/build/lib/libMatterApp.a"
python3 "$M/tools/package.py" "$M" "$CHIP" "$OUT/build" "$OUT/libMatterApp.stripped.a" "$OUT/pkg"     "$OUT/matter-bk7231n-lib.zip"
cd "$OUT"
sha256sum matter-bk7231n-lib.zip > matter-bk7231n-lib.zip.sha256
ls -l "$OUT/build/lib/libMatterApp.a" "$OUT/pkg/lib/libMatterApp.a" matter-bk7231n-lib.zip
cat matter-bk7231n-lib.zip.sha256
