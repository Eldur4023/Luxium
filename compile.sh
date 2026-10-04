#!/usr/bin/env bash
# Builds Luxium (same shape as Lux's compile.sh).
set -euo pipefail
cd "$(dirname "$0")"
cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build -j
echo "binary: build/luxium"
