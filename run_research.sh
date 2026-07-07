#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build_research}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake ..
make -j"$(nproc)"
./CBlockSim
