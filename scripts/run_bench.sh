#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cmake -S "$ROOT" -B "$ROOT/build"
cmake --build "$ROOT/build" -j

"$ROOT/build/nebula" bench --dir "$ROOT/examples"


