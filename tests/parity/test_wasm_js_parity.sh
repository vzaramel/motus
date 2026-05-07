#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
PAGES_FILE="${MOT_PARITY_PAGES_FILE:-$ROOT_DIR/example/parity-pages.txt}"
ORIGIN_PORT_BASE="${MOT_PARITY_ORIGIN_PORT_BASE:-8400}"
EDGE_PORT_BASE="${MOT_PARITY_EDGE_PORT_BASE:-9000}"
TIMEOUT_SEC="${MOT_PARITY_TIMEOUT:-50}"

if [[ "${MOT_E2E:-0}" != "1" ]]; then
  echo "[parity] Skipping JS/WASM parity test (set MOT_E2E=1 to enable)"
  exit 0
fi

echo "[parity] Running target matrix with page fixtures from $PAGES_FILE"
"$ROOT_DIR/example/target-matrix.sh" \
  --pages-file "$PAGES_FILE" \
  --origin-port-base "$ORIGIN_PORT_BASE" \
  --edge-port-base "$EDGE_PORT_BASE" \
  --timeout "$TIMEOUT_SEC"

echo "[parity] JS/WASM parity passed"
