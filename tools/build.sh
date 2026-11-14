#!/bin/bash
# Build the BK7231N libMatterApp.a with the Windows gcc (both chips, natively on Linux: tools/ci_build.sh). Run in WSL:  bash /mnt/c/working/ginhub/matter-espurna/tools/build.sh [out_dir]
# connectedhomeip b87051a95f (Matter 1.5, the commit of esp_matter 1.5.1) at ../connectedhomeip-1.5, zap-cli
# v2025.10.23-nightly in tools/zap-2025.10.23. (The last build on v1.4.2.0 is commit cefb35a of this repository.)
set -e
M=/mnt/c/working/ginhub/matter-espurna
OUT=${1:-$M/out/plug}
CHIP_DIR=/mnt/c/working/ginhub/connectedhomeip-1.5
ZAP_DIR=$M/tools/zap-2025.10.23

PIO_DIR=/mnt/c/Users/igoru/.platformio   # LibreTiny and its BDK packages (headers), the GN root sees //third_party/pio

# gn/third_party/{connectedhomeip,pio}: NTFS junctions made on the Windows side (the Windows gcc can not follow a WSL
# symlink; tools/ci_build.sh makes plain symlinks instead)
junction() {   # <name under gn/third_party> <target under /mnt/c>
    local link=$M/gn/third_party/$1
    if [ "$(readlink "$link" 2>/dev/null)" != "$2" ]; then
        local link_win="C:\\working\\ginhub\\matter-espurna\\gn\\third_party\\$1"
        local target_win="C:/${2#/mnt/c/}"
        target_win="${target_win//\//\\}"
        mkdir -p "$M/gn/third_party"
        /mnt/c/Windows/System32/cmd.exe /c "rmdir $link_win" >/dev/null 2>&1 || rm -f "$link"
        /mnt/c/Windows/System32/cmd.exe /c "mklink /J $link_win $target_win" >/dev/null
    fi
    [ "$(readlink "$link")" = "$2" ] || { echo "gn/third_party/$1 does not point at $2"; exit 1; }
}
junction connectedhomeip "$CHIP_DIR"
junction pio "$PIO_DIR"

# the data model target has to live under connectedhomeip/examples (visibility of src/app:required-privileges)
mkdir -p "$CHIP_DIR/examples/espurna-plug"
cp "$M/gn/chip-examples/espurna-plug/BUILD.gn" "$CHIP_DIR/examples/espurna-plug/BUILD.gn"
python3 "$M/tools/chip_patch.py" "$CHIP_DIR"

echo "connectedhomeip: $(git -C "$CHIP_DIR" log -1 --format='%h %ad' --date=short), zap: $ZAP_DIR"
. $M/.venv-wsl/bin/activate
export PATH=$M/tools/gnroot/usr/bin:$ZAP_DIR:$PATH
export ZAP_INSTALL_PATH=$ZAP_DIR
export PYTHONPATH=$M/gn/third_party/connectedhomeip/scripts/py_matter_idl:$M/gn/third_party/connectedhomeip/scripts/py_matter_yamltests
cd $M/gn
gn gen --check=false "$OUT" --args='import("//chips/bk7231n.gni")'
ninja -j 8 -k 0 -C "$OUT" ${NINJA_TARGETS:-default}

# the package ESPurna takes, unzipped: ESPURNA_MATTER_LIB_DIR=<out>/pkg (lib/, include/ with the headers of the API)
python3 "$M/tools/package.py" bk7231n "$M" "$M/gn/third_party/connectedhomeip" "$OUT" "$OUT/lib/libMatterApp.a" "$OUT/pkg"
