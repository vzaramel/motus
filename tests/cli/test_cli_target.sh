#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT_DIR"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "=== CLI Target Tests ==="

./build/mot \
  --input example/content/pages/wasm.mot \
  --target bytecode \
  --bytecode-out "$TMP_DIR/page.bc" \
  --css-out "$TMP_DIR/page.css" \
  --js-out "$TMP_DIR/page.js"

if [[ ! -s "$TMP_DIR/page.bc" ]]; then
  echo "FAILED: bytecode output was not generated" >&2
  exit 1
fi

./build/mot \
  --input example/content/pages/wasm.mot \
  --target bytecode \
  --no-debug \
  --bytecode-out "$TMP_DIR/page-nodebug.bc"

if [[ ! -s "$TMP_DIR/page-nodebug.bc" ]]; then
  echo "FAILED: no-debug bytecode output was not generated" >&2
  exit 1
fi
if xxd -p "$TMP_DIR/page-nodebug.bc" | tr -d '\n' | grep -q "59444247"; then
  echo "FAILED: no-debug bytecode should not include YDBG trailer magic" >&2
  exit 1
fi

set +e
./build/mot --input example/content/pages/wasm.mot --target wasm --wasm-out "$TMP_DIR/page.wasm" >"$TMP_DIR/wasm.log" 2>&1
WASM_RC=$?
set -e

if [[ "$WASM_RC" -eq 0 ]]; then
  echo "FAILED: wasm target should fail until compiler target is implemented" >&2
  exit 1
fi
if ! grep -q "WASM target is not implemented" "$TMP_DIR/wasm.log"; then
  echo "FAILED: wasm target failure message missing" >&2
  cat "$TMP_DIR/wasm.log"
  exit 1
fi

set +e
./build/mot --input example/content/pages/wasm.mot --target both --bytecode-out "$TMP_DIR/page2.bc" --wasm-out "$TMP_DIR/page2.wasm" >"$TMP_DIR/both.log" 2>&1
BOTH_RC=$?
set -e

if [[ "$BOTH_RC" -eq 0 ]]; then
  echo "FAILED: both target should fail until compiler target is implemented" >&2
  exit 1
fi
if ! grep -q "WASM target is not implemented" "$TMP_DIR/both.log"; then
  echo "FAILED: both target failure message missing" >&2
  cat "$TMP_DIR/both.log"
  exit 1
fi

echo "CLI target tests passed"
