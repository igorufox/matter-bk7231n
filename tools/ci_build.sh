#!/bin/bash
# Native Linux build of libMatterApp.a for every chip and the release packages (GitHub Actions, or Linux / WSL by
# hand): the same connectedhomeip, plug.zap data model and application, the platform of the chip under it
#
#   tools/ci_build.sh <deps_dir> <out_dir>
#
# <deps_dir> gets everything the build needs, at the versions pinned below (skipped when already there, so it can be
# cached): connectedhomeip with the submodules the library uses, a Python venv for the code generators, zap-cli;
# BK7231N: LibreTiny + the BDK packages it uses (headers only) and the Linux build of the arm-none-eabi gcc LibreTiny
# compiles the firmware with (PlatformIO package, PLATFORMIO_CORE_DIR=<deps_dir>/pio);
# ESP32: the xtensa-esp-elf gcc and the prebuilt ESP-IDF (headers, flags, sdkconfig) of the Arduino-ESP32 release the
# ESP32 build of ESPurna uses.
# <out_dir> gets the ninja builds (build-<chip>/) and a package per chip:
#   matter-espurna-<chip>.zip         lib/libMatterApp.a (debug info stripped), the headers of the library's API with
#                                     their flags (include/chip), bk7231n also include/lwipopts.h (tools/package.py)
#   matter-espurna-<chip>.zip.sha256  "<sha256>  matter-espurna-<chip>.zip"
#
# Needs git, curl, unzip, xz, python3 with venv and gn on PATH (Ubuntu 24.04: apt install generate-ninja). The Windows
# gcc build of the BK7231N one from WSL is tools/build.sh; the same GN root, this one with bk_toolchain = "native".
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
# BK7231N: GCC 14.2.1 (Arm GNU Toolchain 14.2.Rel1), the one ESPurna's Beken build takes instead of LibreTiny's 10.3
# (ESPurna libraries.ini beken_toolchain, U25): one compiler for the firmware and this library
GCC_PACKAGE=platformio/toolchain-gccarmnoneeabi@1.140201.0
GCC_VERSION=14.2.1
# ESP32: the gcc and the prebuilt ESP-IDF 5.5 of Arduino-ESP32 3.3.12 (pioarduino platform 55.03.312 of ESPurna's
# libraries.ini: framework-arduinoespressif32-libs, toolchain-xtensa-esp-elf 14.2.0+20260121)
ESP32_GCC_URL=https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20260121/xtensa-esp-elf-14.2.0_20260121-x86_64-linux-gnu.tar.xz
ESP32_GCC_VERSION=14.2.0
ESP32_LIBS_URL=https://github.com/espressif/arduino-esp32/releases/download/3.3.12/esp32-core-3.3.12-libs.tar.xz
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
ESP32_GCC=$DEPS/xtensa-esp-elf
ESP32_LIBS=$DEPS/esp32-libs

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
# a cache of an older pin (restore-keys) has another gcc there: replaced, not kept
if [ -x "$GCC_BIN/arm-none-eabi-gcc" ] && ! "$GCC_BIN/arm-none-eabi-gcc" --version | head -1 | grep -q " $GCC_VERSION "; then
    rm -rf "$PIO/packages/toolchain-gccarmnoneeabi"
fi
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

# a tar.xz unpacked into <dir> once; .ci-url marks a finished one
fetch_tar() {   # <dir> <url>
    if [ "$(cat "$1/.ci-url" 2>/dev/null)" = "$2" ]; then
        return
    fi
    echo "== $2"
    rm -rf "$1" "$1.tar.xz"
    mkdir -p "$1"
    curl -fsSL -o "$1.tar.xz" "$2"
    tar -xJf "$1.tar.xz" -C "$1"
    rm "$1.tar.xz"
    echo "$2" > "$1/.ci-url"
}

fetch_tar "$ESP32_GCC" "$ESP32_GCC_URL"
ESP32_BIN=$(dirname "$(find "$ESP32_GCC" -path "*/bin/xtensa-esp32-elf-gcc" | head -1)")
"$ESP32_BIN/xtensa-esp32-elf-gcc" --version | head -1 | grep -q " $ESP32_GCC_VERSION" || {
    echo "xtensa-esp32-elf-gcc is not $ESP32_GCC_VERSION:"; "$ESP32_BIN/xtensa-esp32-elf-gcc" --version | head -1; exit 1; }

# <libs>/esp32 with flags/, include/ and dio_qspi/include/sdkconfig.h
fetch_tar "$ESP32_LIBS" "$ESP32_LIBS_URL"
ESP32_SDK=$(dirname "$(dirname "$(find "$ESP32_LIBS" -path "*/esp32/flags/includes" | head -1)")")
[ -f "$ESP32_SDK/dio_qspi/include/sdkconfig.h" ] || { echo "no esp32/dio_qspi/include/sdkconfig.h in $ESP32_LIBS_URL"; exit 1; }

command -v gn > /dev/null || { echo "gn is not on PATH (Ubuntu: apt install generate-ninja)"; exit 1; }

# the GN root finds connectedhomeip and the PlatformIO packages through these links (tools/build.sh makes NTFS
# junctions instead, for the Windows gcc)
mkdir -p "$M/gn/third_party"
for link in "connectedhomeip:$CHIP" "pio:$PIO" "esp32-toolchain:$(dirname "$ESP32_BIN")" "esp32-libs:$ESP32_SDK"; do
    name=${link%%:*}
    target=${link#*:}
    rm -rf "${M:?}/gn/third_party/$name"
    ln -s "$target" "$M/gn/third_party/$name"
done
# the data model target has to live under connectedhomeip/examples (visibility of src/app:required-privileges)
mkdir -p "$CHIP/examples/espurna-plug"
cp "$M/gn/chip-examples/espurna-plug/BUILD.gn" "$CHIP/examples/espurna-plug/BUILD.gn"
# the flags of the prebuilt ESP-IDF for the ESP32 build
python3 "$M/tools/gen_esp32_flags.py" "$ESP32_SDK" "$M/gn/chips/esp32_flags.gni"

# shellcheck disable=SC1091
. "$VENV/bin/activate"
export PATH=$ZAP:$PATH
export ZAP_INSTALL_PATH=$ZAP
export PYTHONPATH=$CHIP/scripts/py_matter_idl:$CHIP/scripts/py_matter_yamltests

echo "== build: connectedhomeip $(git -C "$CHIP" log -1 --format='%h %ad' --date=short)," \
     "$("$GCC_BIN/arm-none-eabi-gcc" --version | head -1), $("$ESP32_BIN/xtensa-esp32-elf-gcc" --version | head -1)," \
     "gn $(gn --version), ninja $(ninja --version)"
cd "$M/gn"

# one chip: <chip> <gn args> <strip>
build() {
    echo "== build $1"
    # no include check (`--check`, any value of it turns it on): connectedhomeip's ESP32 platform includes headers of
    # targets it does not depend on (OTAImageProcessorImpl.h: ota-requestor), its own ESP-IDF build does not check
    gn gen "$OUT/build-$1" --args="$2"
    ninja -j "$JOBS" -C "$OUT/build-$1" default

    echo "== package $1"
    "$3" --strip-debug -o "$OUT/libMatterApp-$1.stripped.a" "$OUT/build-$1/lib/libMatterApp.a"
    python3 "$M/tools/package.py" "$1" "$M" "$CHIP" "$OUT/build-$1" "$OUT/libMatterApp-$1.stripped.a" "$OUT/pkg-$1" \
        "$OUT/matter-espurna-$1.zip"
    (cd "$OUT" && sha256sum "matter-espurna-$1.zip" > "matter-espurna-$1.zip.sha256")
    ls -l "$OUT/build-$1/lib/libMatterApp.a" "$OUT/pkg-$1/lib/libMatterApp.a" "$OUT/matter-espurna-$1.zip"
    cat "$OUT/matter-espurna-$1.zip.sha256"
}

build bk7231n 'import("//chips/bk7231n.gni") bk_toolchain="native"' "$GCC_BIN/arm-none-eabi-strip"
build esp32 'import("//chips/esp32.gni")' "$ESP32_BIN/xtensa-esp32-elf-strip"
