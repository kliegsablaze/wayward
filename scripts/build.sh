#!/usr/bin/env bash
# Builds wayward.so for aarch64 (Ableton Move) and packages dist/wayward-module.tar.gz.
#
# Set CROSS_PREFIX to override the toolchain prefix (defaults to
# aarch64-linux-gnu-, matching the Docker cross-compilation environment in
# scripts/Dockerfile). Run natively on an aarch64 host by setting
# CROSS_PREFIX= (empty).
set -euo pipefail

cd "$(dirname "$0")/.."

CROSS_PREFIX="${CROSS_PREFIX-aarch64-linux-gnu-}"

if ! command -v "${CROSS_PREFIX}gcc" >/dev/null 2>&1; then
    echo "Error: missing compiler '${CROSS_PREFIX}gcc'" >&2
    echo "Run inside scripts/Dockerfile's container, or set CROSS_PREFIX=" \
         "for a native build." >&2
    exit 1
fi

echo "=== Building wayward (target: ${CROSS_PREFIX:-native}) ==="

rm -rf dist
mkdir -p dist/wayward

"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/dsp/wayward.c \
    -o dist/wayward/wayward.so \
    -Isrc/dsp \
    -lm

cp src/module.json dist/wayward/module.json

# The host scans installed module directories for help.json at runtime and
# needs no manifest entry, so the only thing that makes it reach a device is
# being in the tarball.
[ -f src/help.json ] && cp src/help.json dist/wayward/help.json

cd dist
tar -czvf wayward-module.tar.gz wayward/
cd ..

echo ""
echo "Tarball: dist/wayward-module.tar.gz"
