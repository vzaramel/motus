# Motus Project Structure

This document reflects the current repository layout and implementation status.

## Directory Layout

```text
motus/
├── src/
│   ├── mot.h, mot.c                  # Public API
│   ├── cli/                          # CLI frontend (`build/mot`)
│   ├── lexer/                        # Tokenizer
│   ├── parser/                       # AST + parser
│   ├── analyzer/                     # Semantic analysis + deps
│   ├── compiler/                     # Bytecode compiler + partial eval + serdes
│   ├── runtime/                      # Shared C VM
│   ├── debug/                        # Source map helpers (VLQ + v3 JSON)
│   ├── codegen/                      # CSS/JS extraction and scoping
│   └── util/                         # Arena/hash/vec/string utilities
│
├── runtime-wasm/
│   ├── src/wasm_vm.c                 # Freestanding wasm adapter reusing shared VM
│   ├── src/freestanding/             # Minimal libc replacements
│   ├── src/host.js                   # JS host adapter for wasm runtime
│   └── build.sh                      # wasm build script
│
├── browser-runtime/
│   ├── src/runtime-contract.js       # Browser-side contract helpers (parity baseline)
│   ├── src/host.js                   # Browser host adapter (data/component RPC)
│   ├── test-runtime-contract.mjs     # Contract snapshot/parity test
│   └── test-host.mjs                 # Browser host behavior tests
│
├── example/
│   ├── origin/                       # C origin server with startup precompile cache
│   ├── edge/                         # Cloudflare Worker runtime
│   │   ├── src/worker.js
│   │   ├── src/interpreter.js        # Legacy JS interpreter (not used by edge runtime)
│   │   ├── src/wasm-vm.js            # WASM VM bridge
│   │   ├── src/runtime-contract.js   # Canonical JS runtime contract helpers
│   │   ├── src/render-trace.js       # Dev trace schema/timings
│   │   ├── src/bytecode-debug.js     # Bytecode debug metadata/source-map extraction
│   │   ├── src/browser-wasm-inline.js # Browser-side wasm runtime bootstrap script generator
│   │   └── src/devtools-inline.js    # In-page debug sidebar bootstrap
│   ├── precompile-system-components.mjs
│   ├── e2e-wrangler.sh
│   ├── e2e-debug.sh
│   ├── target-matrix.sh
│   ├── parity-pages.txt
│   └── run.sh
│
├── tests/
│   ├── lexer/
│   ├── parser/
│   ├── analyzer/
│   ├── compiler/
│   ├── runtime/
│   ├── codegen/
│   ├── api/
│   ├── cli/
│   ├── debug/
│   └── parity/                       # e2e parity wrapper (opt-in via env)
│
├── DESIGN.md
├── IMPLEMENTATION_PLAN.md
├── CODEX.md
└── Makefile
```

## Test Summary

Current `make test` coverage:

| Suite | Tests |
|---|---:|
| Lexer | 9 |
| Parser | 19 |
| Analyzer | 17 |
| Compiler | 19 |
| Partial Eval | 18 |
| VM | 37 |
| Codegen | 10 |
| API | 11 |
| CLI | 1 |
| Debug metadata | 2 |
| Source map | 5 |
| Host contract | 2 |
| **Total** | **150** |

Additional opt-in e2e suite:
- `make test_wasm_parity` (requires `MOT_E2E=1`, runs `example/target-matrix.sh` with fixture pages)

## Build and Run

Core:

```bash
make lib
make test
make cli
```

Example:

```bash
cd example
./run.sh --origin-port 8091 --edge-port 8791 --vm wasm
```

WASM smoke across fixture pages:

```bash
cd example
./target-matrix.sh --pages-file parity-pages.txt --origin-port-base 8200 --edge-port-base 8900
```

## Target Architecture Status

### Implemented now

- Bytecode compile target is production path in core compiler/API.
- Shared C VM is reused by runtime-wasm build (no separate interpreter logic drift).
- Edge runtime currently runs in WASM-only mode (`wasm`).
- Startup precompile cache exists on origin for pages and components.
- Edge can bundle precompiled system components and component source-map artifacts.
- Development trace payload includes module/query/timing metadata and debug source-map payloads.
- Compiler emits direct/attribute/expression reactive marker sets:
  `@bind/@set/@plan`, `@bindattr/@planattr`, `@exprbind/@exprattr/@exprdep/@planexpr`.
- Browser runtime builds reactive indexes from bytecode dependency headers and applies targeted text/attribute updates.
- Edge generates and embeds per-page reactive wasm companion payloads for numeric expression updates.
- Browser runtime loads per-page reactive wasm exports and falls back to JS evaluator for unsupported expressions.
- Browser runtime uses same-origin edge RPC proxy endpoints:
  `/_mot/runtime/wasm`, `/_mot/page/{name}`, `/_mot/component/{name}`, `/_mot/data/{page}`.

### In progress / not complete yet

- Core compiler `--target wasm|both` currently returns an explicit not-implemented error.
- True wasm instruction-offset source-map emission is pending toolchain integration.
- Mixed execution inside one render (VM invoking predeployed wasm components directly) is not complete.
- Browser/editor native wasm runtime bootstrap exists (enabled by default in example runner, can still be toggled with `MOT_BROWSER_WASM` or `?browser_wasm=1`)
  and executes page/component bytecode through the shared C runtime wasm module in-browser.
- Browser/editor reactive update loop is partially implemented (path + supported expression classes);
  full expression coverage and source-map driven debugger integration are still incomplete.

## Contract and Runtime Notes

- Data RPC contract is `POST /data/{page}` with payload:
  - `queryRef`
  - `signature`
  - `params`
  - `single`
- Bytecode v1.2 stores query metadata only (no raw SQL text).
- Edge and runtime-wasm JS hosts normalize `u32` fields to avoid signed mismatch issues.
- Debug mode can embed `mot-trace-data` and inline devtools bootstrap for in-browser inspection.

## Current Source Of Truth

- Architecture and goals: `DESIGN.md`
- Work sequencing and acceptance: `IMPLEMENTATION_PLAN.md`
- Fast operational notes: `CODEX.md`
