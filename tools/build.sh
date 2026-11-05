#!/bin/bash
# Build libMatterApp.a for BK7231N. Run in WSL:  bash /mnt/c/working/ginhub/matter-bk7231n/tools/build.sh [out_dir]
set -e
M=/mnt/c/working/ginhub/matter-bk7231n
OUT=${1:-$M/out/plug}
. $M/.venv-wsl/bin/activate
export PATH=$M/tools/gnroot/usr/bin:$M/tools/zap:$PATH
export ZAP_INSTALL_PATH=$M/tools/zap
export PYTHONPATH=$M/gn/third_party/connectedhomeip/scripts/py_matter_idl:$M/gn/third_party/connectedhomeip/scripts/py_matter_yamltests
cd $M/gn
gn gen --check=false "$OUT"
ninja -k 0 -C "$OUT" ${NINJA_TARGETS:-default}
