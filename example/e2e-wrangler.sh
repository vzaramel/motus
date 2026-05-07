#!/usr/bin/env bash
#
# End-to-end verification using Wrangler + local origin server.
#
# This script:
# 1. Builds core lib + origin server + wasm runtime
# 2. Starts origin server on a configurable port
# 3. Starts wrangler dev on a configurable edge port using WASM VM mode
# 4. Checks health endpoints and fetches one page
# 5. Verifies runtime marker in HTML and exits non-zero on failure
#
# Example:
#   ./e2e-wrangler.sh --origin-port 8091 --edge-port 8792 --vm wasm --page wasm
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ORIGIN_PORT="8080"
EDGE_PORT="8791"
VM_MODE="wasm"
PAGE="wasm"
TIMEOUT_SEC="30"
SKIP_BUILD="0"
DEBUG_MODE="0"
OUTPUT_HTML=""

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --origin-port <port>  Origin server port (default: 8080)
  --edge-port <port>    Edge worker port (default: 8791)
  --vm <mode>           VM mode: wasm (default: wasm)
  --page <name>         Page path to fetch (default: wasm)
  --timeout <seconds>   Startup timeout in seconds (default: 30)
  --skip-build          Skip core/origin build steps and use existing binaries
  --debug               Enable MOT_DEBUG, build wasm runtime in debug mode, and assert trace payload in HTML
  --browser-wasm        Reserved (browser wasm/reactivity temporarily disabled)
  --output-html <path>  Write fetched HTML response to file
  --help                Show this help
EOF
}

is_valid_port() {
  local p="$1"
  [[ "$p" =~ ^[0-9]+$ ]] && (( p >= 1 && p <= 65535 ))
}

wait_for_url() {
  local url="$1"
  local timeout="$2"
  local i
  for ((i=0; i<timeout; i++)); do
    if curl -fsS "$url" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
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
    --debug)
      DEBUG_MODE="1"
      shift
      ;;
    --browser-wasm)
      echo "[e2e] Browser wasm/reactivity is temporarily disabled; ignoring --browser-wasm"
      shift
      ;;
    --output-html)
      OUTPUT_HTML="${2:-}"
      shift 2
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

if ! is_valid_port "$ORIGIN_PORT"; then
  echo "Invalid --origin-port: $ORIGIN_PORT" >&2
  exit 1
fi

if ! is_valid_port "$EDGE_PORT"; then
  echo "Invalid --edge-port: $EDGE_PORT" >&2
  exit 1
fi

if [[ "$VM_MODE" != "wasm" ]]; then
  echo "Invalid --vm: $VM_MODE (expected wasm)" >&2
  exit 1
fi

if ! [[ "$TIMEOUT_SEC" =~ ^[0-9]+$ ]] || (( TIMEOUT_SEC < 1 )); then
  echo "Invalid --timeout: $TIMEOUT_SEC" >&2
  exit 1
fi

LOG_DIR="$SCRIPT_DIR/.logs"
mkdir -p "$LOG_DIR"
ORIGIN_LOG="$LOG_DIR/origin-e2e.log"
EDGE_LOG="$LOG_DIR/edge-e2e.log"

ORIGIN_PID=""
EDGE_PID=""

cleanup() {
  if [[ -n "$EDGE_PID" ]]; then
    kill "$EDGE_PID" >/dev/null 2>&1 || true
  fi
  if [[ -n "$ORIGIN_PID" ]]; then
    kill "$ORIGIN_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

if [[ "$SKIP_BUILD" == "0" ]]; then
  echo "[e2e] Building core library"
  make -C "$ROOT_DIR" lib >/dev/null

  echo "[e2e] Building origin server"
  make -C "$SCRIPT_DIR/origin" >/dev/null
else
  echo "[e2e] Skipping core/origin build steps"
fi

echo "[e2e] Installing edge deps (if needed)"
(cd "$SCRIPT_DIR/edge" && npm install --silent >/dev/null 2>&1 || true)

echo "[e2e] Precompiling system components"
(cd "$SCRIPT_DIR/edge" && npm run build:system-components >/dev/null)

echo "[e2e] Building wasm runtime"
WASM_BUILD_SCRIPT="build:wasm"
if [[ "$DEBUG_MODE" == "1" ]]; then
  WASM_BUILD_SCRIPT="build:wasm:debug"
fi
(cd "$SCRIPT_DIR/edge" && npm run "$WASM_BUILD_SCRIPT" >/dev/null)

echo "[e2e] Starting origin on :$ORIGIN_PORT"
(
  cd "$SCRIPT_DIR/origin"
  exec env ORIGIN_PORT="$ORIGIN_PORT" ./server >"$ORIGIN_LOG" 2>&1
) &
ORIGIN_PID=$!

if ! wait_for_url "http://127.0.0.1:${ORIGIN_PORT}/health" "$TIMEOUT_SEC"; then
  echo "[e2e] Origin did not become ready in ${TIMEOUT_SEC}s" >&2
  echo "[e2e] Origin log: $ORIGIN_LOG" >&2
  exit 1
fi
if ! kill -0 "$ORIGIN_PID" >/dev/null 2>&1; then
  echo "[e2e] Origin process exited early (possible port conflict)" >&2
  echo "[e2e] Origin log: $ORIGIN_LOG" >&2
  exit 1
fi

echo "[e2e] Starting wrangler on :$EDGE_PORT (vm=$VM_MODE)"
WRANGLER_DEBUG_VAR=""
if [[ "$DEBUG_MODE" == "1" ]]; then
  WRANGLER_DEBUG_VAR="--var MOT_DEBUG:1"
fi
(
  cd "$SCRIPT_DIR/edge"
  exec npx wrangler dev --local \
    --port "$EDGE_PORT" \
    --var "ORIGIN_PORT:${ORIGIN_PORT}" \
    --var "ORIGIN_URL:http://localhost:${ORIGIN_PORT}" \
    --var "MOT_VM:${VM_MODE}" \
    $WRANGLER_DEBUG_VAR >"$EDGE_LOG" 2>&1
) &
EDGE_PID=$!

if ! wait_for_url "http://127.0.0.1:${EDGE_PORT}/health" "$TIMEOUT_SEC"; then
  echo "[e2e] Edge did not become ready in ${TIMEOUT_SEC}s" >&2
  echo "[e2e] Edge log: $EDGE_LOG" >&2
  exit 1
fi
if ! kill -0 "$EDGE_PID" >/dev/null 2>&1; then
  echo "[e2e] Edge process exited early" >&2
  echo "[e2e] Edge log: $EDGE_LOG" >&2
  exit 1
fi

HEALTH_JSON="$(curl -fsS "http://127.0.0.1:${EDGE_PORT}/health")"
HTML="$(curl -fsS "http://127.0.0.1:${EDGE_PORT}/${PAGE}")"

echo "[e2e] Edge health: $HEALTH_JSON"

if grep -q "<h1>Error</h1>" <<<"$HTML"; then
  echo "[e2e] Render returned error page" >&2
  echo "[e2e] Edge log: $EDGE_LOG" >&2
  echo "[e2e] Origin log: $ORIGIN_LOG" >&2
  exit 1
fi

if ! grep -q "Motus Edge Runtime" <<<"$HTML"; then
  echo "[e2e] Missing runtime marker in HTML output" >&2
  exit 1
fi

if ! grep -q "vm: wasm" <<<"$HTML"; then
  echo "[e2e] Expected vm marker vm: wasm not found" >&2
  exit 1
fi

if [[ "$DEBUG_MODE" == "1" ]]; then
  if ! grep -q 'id="mot-trace-data"' <<<"$HTML"; then
    echo "[e2e] Expected debug trace payload script not found" >&2
    exit 1
  fi
  if ! grep -q 'mot-debug-toggle' <<<"$HTML"; then
    echo "[e2e] Expected debug devtools bootstrap script not found" >&2
    exit 1
  fi
  if ! grep -q '"bytecodeDebug"' <<<"$HTML"; then
    echo "[e2e] Expected bytecodeDebug payload not found in trace JSON" >&2
    exit 1
  fi
  if ! grep -q '"sourceMap":{"version":3' <<<"$HTML"; then
    echo "[e2e] Expected sourceMap payload not found in trace JSON" >&2
    exit 1
  fi
  if ! grep -q '"componentSourceMaps"' <<<"$HTML"; then
    echo "[e2e] Expected componentSourceMaps payload not found in trace JSON" >&2
    exit 1
  fi
  if ! grep -q '"executionPaths"' <<<"$HTML"; then
    echo "[e2e] Expected executionPaths payload not found in trace JSON" >&2
    exit 1
  fi
fi

if grep -q 'id="mot-browser-wasm-bootstrap"' <<<"$HTML"; then
  echo "[e2e] Browser wasm bootstrap script should be disabled but was found" >&2
  exit 1
fi

if [[ -n "$OUTPUT_HTML" ]]; then
  mkdir -p "$(dirname "$OUTPUT_HTML")"
  printf "%s" "$HTML" > "$OUTPUT_HTML"
  echo "[e2e] Wrote HTML output to $OUTPUT_HTML"
fi

echo "[e2e] Render check passed for /${PAGE}"
echo "[e2e] Logs: $ORIGIN_LOG, $EDGE_LOG"
echo "[e2e] HTML tail:"
echo "$HTML" | tail -n 5
