#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

bin="build/tests/test_wayward"
mkdir -p "$(dirname "$bin")"

cc -std=c11 -Wall -Wextra -Werror \
  -Isrc/dsp \
  tests/test_wayward.c \
  src/dsp/wayward.c \
  -lm \
  -o "$bin"

"$bin"
