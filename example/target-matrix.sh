#!/usr/bin/env bash
#
# WASM target smoke checks across pages.
#
# Runs local e2e in wasm mode for one or more pages.
#
# Example:
#   ./target-matrix.sh --page index --origin-port-base 8200 --edge-port-base 8900
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP_DIR="$SCRIPT_DIR/.logs/target-matrix"

PAGE="index"
PAGES_FILE=""
TIMEOUT_SEC="50"
ORIGIN_PORT_BASE="8200"
EDGE_PORT_BASE="8900"

usage() {
  cat <<EOF
Usage: $0 [options]

Options:
  --page <name>               Page path to fetch (default: index)
  --pages-file <path>         File with one page per line (overrides --page)
  --timeout <seconds>         Startup timeout in seconds (default: 50)
  --origin-port-base <port>   Base port for origin runs (default: 8200)
  --edge-port-base <port>     Base port for edge runs (default: 8900)
  --help                      Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --page)
      PAGE="${2:-}"
      shift 2
      ;;
    --pages-file)
      PAGES_FILE="${2:-}"
      shift 2
      ;;
    --timeout)
      TIMEOUT_SEC="${2:-}"
      shift 2
      ;;
    --origin-port-base)
      ORIGIN_PORT_BASE="${2:-}"
      shift 2
      ;;
    --edge-port-base)
      EDGE_PORT_BASE="${2:-}"
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

mkdir -p "$TMP_DIR"

run_case() {
  local page="$1"
  local origin_port="$2"
  local edge_port="$3"
  local out_file="$4"
  local skip_build="$5"
  local cmd=(
    "$SCRIPT_DIR/e2e-wrangler.sh"
    --origin-port "$origin_port"
    --edge-port "$edge_port"
    --vm wasm
    --page "$page"
    --timeout "$TIMEOUT_SEC"
    --output-html "$out_file"
  )

  if [[ "$skip_build" == "1" ]]; then
    cmd+=(--skip-build)
  fi

  echo "[matrix] Running mode=wasm page=$page origin=$origin_port edge=$edge_port"
  "${cmd[@]}"
}

read_pages() {
  local pages=()
  if [[ -n "$PAGES_FILE" ]]; then
    if [[ ! -f "$PAGES_FILE" ]]; then
      echo "Pages file does not exist: $PAGES_FILE" >&2
      exit 1
    fi
    while IFS= read -r line; do
      local trimmed
      trimmed="$(printf '%s' "$line" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
      if [[ -z "$trimmed" || "$trimmed" == \#* ]]; then
        continue
      fi
      pages+=("$trimmed")
    done <"$PAGES_FILE"
  else
    pages+=("$PAGE")
  fi

  if [[ "${#pages[@]}" -eq 0 ]]; then
    echo "No pages to test" >&2
    exit 1
  fi
  printf '%s\n' "${pages[@]}"
}

PORT_OFFSET=0
SKIP_BUILD="0"

while IFS= read -r current_page; do
  WASM_HTML="$TMP_DIR/${current_page}.wasm.html"

  run_case "$current_page" "$((ORIGIN_PORT_BASE + PORT_OFFSET))" "$((EDGE_PORT_BASE + PORT_OFFSET))" "$WASM_HTML" "$SKIP_BUILD"
  PORT_OFFSET=$((PORT_OFFSET + 1))
  SKIP_BUILD="1"

  echo "[matrix] Page '$current_page' passed"
  echo "[matrix] Artifact: $WASM_HTML"
done < <(read_pages)

echo "[matrix] WASM target checks passed"
