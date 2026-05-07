#!/usr/bin/env bash
#
# Debug-focused e2e wrapper.
#
# Validates that dev/debug responses include trace metadata and
# source-map payload for browser tooling.
#
# Example:
#   ./e2e-debug.sh --mode trace --origin-port 8091 --edge-port 8798 --vm wasm --page layout
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

MODE="trace"
ORIGIN_PORT="8091"
EDGE_PORT="8798"
VM_MODE="wasm"
PAGE="layout"
TIMEOUT_SEC="50"
SKIP_BUILD="0"

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --mode <mode>         Debug mode: trace (default: trace)
  --origin-port <port>  Origin server port (default: 8091)
  --edge-port <port>    Edge worker port (default: 8798)
  --vm <mode>           VM mode: wasm (default: wasm)
  --page <name>         Page path to fetch (default: layout)
  --timeout <seconds>   Startup timeout in seconds (default: 50)
  --skip-build          Skip core/origin build steps
  --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      MODE="${2:-}"
      shift 2
      ;;
    --origin-port)
      ORIGIN_PORT="${2:-}"
      shift 2
      ;;
    --edge-port)
      EDGE_PORT="${2:-}"
      shift 2
      ;;
    --vm)
      VM_MODE="${2:-}"
      shift 2
      ;;
    --page)
      PAGE="${2:-}"
      shift 2
      ;;
    --timeout)
      TIMEOUT_SEC="${2:-}"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD="1"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "$MODE" != "trace" ]]; then
  echo "Unsupported --mode: $MODE (expected trace)" >&2
  exit 1
fi

if [[ "$VM_MODE" != "wasm" ]]; then
  echo "Unsupported --vm: $VM_MODE (expected wasm)" >&2
  exit 1
fi

CMD=(
  "$SCRIPT_DIR/e2e-wrangler.sh"
  --origin-port "$ORIGIN_PORT"
  --edge-port "$EDGE_PORT"
  --vm "$VM_MODE"
  --page "$PAGE"
  --timeout "$TIMEOUT_SEC"
  --debug
)

if [[ "$SKIP_BUILD" == "1" ]]; then
  CMD+=(--skip-build)
fi

echo "[e2e-debug] Running mode=$MODE vm=$VM_MODE page=$PAGE origin=$ORIGIN_PORT edge=$EDGE_PORT"
exec "${CMD[@]}"
