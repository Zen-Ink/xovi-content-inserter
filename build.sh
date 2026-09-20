#!/usr/bin/env bash
set -euo pipefail

project=$(cd -- "$(dirname -- "$0")" && pwd)
workspace=$(cd -- "$project/.." && pwd)
toolchain=${TOOLCHAIN_ROOT:-"$workspace/toolchain"}
export XOVI_REPO=${XOVI_REPO:-"$workspace/external/xovi"}

case "${1:-all}" in
    all) targets=(aarch64 arm32) ;;
    aarch64|arm32) targets=("$1") ;;
    *) echo "Usage: $0 [all|aarch64|arm32]" >&2; exit 2 ;;
esac
for arch in "${targets[@]}"; do (
    # Yocto environment hooks reference optional unset variables.
    set +u
    if [ "$arch" = aarch64 ]; then
        source "$toolchain/rmpp/environment-setup-cortexa53-crypto-remarkable-linux"
    else
        source "$toolchain/rm2/environment-setup-cortexa7hf-neon-remarkable-linux-gnueabi"
    fi
    set -u
    cd "$project"
    python3 "$XOVI_REPO/util/xovigen.py" -o xovi.cpp -H xovi.h xovi-content-inserter.xovi
    mkdir -p "build/$arch"
    qmake -o "build/$arch/Makefile" xovi-content-inserter.pro CONFIG+=release
    make -C "build/$arch" -j"${JOBS:-4}"
); done
