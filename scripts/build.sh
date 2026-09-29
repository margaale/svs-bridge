#!/usr/bin/env bash
# Local build of a target (src/platform/<target>) into build/<target>, as in Cruller. Only esp32 for
# now; run it in an ESP-IDF 6.1 shell (idf.py on the PATH).
#   [SVS_BRIDGE_VERSION=x.y.z] scripts/build.sh [esp32]
set -euo pipefail
cd "$(dirname "$0")/.."

target=esp32
case "${1:-}" in esp32) target=$1; shift ;; esac
src="src/platform/$target"
out="build/$target"

# Optimization comes from sdkconfig.defaults, not a build type.
idf.py -C "$src" -B "$out" ${SVS_BRIDGE_VERSION:+-DSVS_BRIDGE_VERSION="$SVS_BRIDGE_VERSION"} reconfigure
ninja -C "$out" -k 0 # every error at once, not just the first
# A new board over USB: bootloader, partition table, OTA data and app in one file, at 0x0.
idf.py -C "$src" -B "$out" merge-bin -o "$PWD/$out/svs_bridge-factory.bin"
ls -l "$out/svs_bridge.bin" "$out/svs_bridge-factory.bin"
