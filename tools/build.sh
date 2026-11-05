#!/bin/bash
# Build libMatterApp.a for BK7231N. Run in WSL:  bash /mnt/c/working/ginhub/matter-bk7231n/tools/build.sh [out_dir]
# connectedhomeip b87051a95f (Matter 1.5, the commit of esp_matter 1.5.1) at ../connectedhomeip-1.5, zap-cli
# v2025.10.23-nightly in tools/zap-2025.10.23. (The last build on v1.4.2.0 is commit cefb35a of this repository.)
set -e
M=/mnt/c/working/ginhub/matter-bk7231n
OUT=${1:-$M/out/plug}
CHIP_DIR=/mnt/c/working/ginhub/connectedhomeip-1.5
ZAP_DIR=$M/tools/zap-2025.10.23

# gn/third_party/connectedhomeip: an NTFS junction made on the Windows side (the Windows gcc can not follow a WSL
# symlink)
LINK=$M/gn/third_party/connectedhomeip
if [ "$(readlink "$LINK" 2>/dev/null)" != "$CHIP_DIR" ]; then
    LINK_WIN='C:\working\ginhub\matter-bk7231n\gn\third_party\connectedhomeip'
    TARGET_WIN="C:/${CHIP_DIR#/mnt/c/}"
    TARGET_WIN="${TARGET_WIN//\//\\}"
    /mnt/c/Windows/System32/cmd.exe /c "rmdir $LINK_WIN" >/dev/null 2>&1 || rm -f "$LINK"
    /mnt/c/Windows/System32/cmd.exe /c "mklink /J $LINK_WIN $TARGET_WIN" >/dev/null
fi
[ "$(readlink "$LINK")" = "$CHIP_DIR" ] || { echo "gn/third_party/connectedhomeip does not point at $CHIP_DIR"; exit 1; }

# the data model target has to live under connectedhomeip/examples (visibility of src/app:required-privileges)
mkdir -p "$CHIP_DIR/examples/bk7231n-plug"
cp "$M/gn/chip-examples/bk7231n-plug/BUILD.gn" "$CHIP_DIR/examples/bk7231n-plug/BUILD.gn"

echo "connectedhomeip: $(git -C "$CHIP_DIR" log -1 --format='%h %ad' --date=short), zap: $ZAP_DIR"
. $M/.venv-wsl/bin/activate
export PATH=$M/tools/gnroot/usr/bin:$ZAP_DIR:$PATH
export ZAP_INSTALL_PATH=$ZAP_DIR
export PYTHONPATH=$M/gn/third_party/connectedhomeip/scripts/py_matter_idl:$M/gn/third_party/connectedhomeip/scripts/py_matter_yamltests
cd $M/gn
gn gen --check=false "$OUT"
ninja -j 8 -k 0 -C "$OUT" ${NINJA_TARGETS:-default}
