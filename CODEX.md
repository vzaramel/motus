# CODEX.md

This file is the fast-start guide for working in this repository.

## What this project is

Motus is a C99 library and toolchain for an XML-based templating language that compiles `.mot` source to streamable bytecode plus extracted CSS/JS.

Primary flow:
1. `lexer` -> `parser` -> AST
2. `analyzer` -> scopes/types/dependencies
3. `compiler` -> bytecode module
4. `runtime`/`runtime-wasm` -> streaming HTML output

Reference docs:
- `DESIGN.md`
- `PROJECT_STRUCTURE.md`
- `IMPLEMENTATION_PLAN.md`

## Current state (important)

- Core compile/runtime path is working and covered by tests.
- Public API symbols are implemented in `src/mot.c`:
  - `mot_compile`
  - `mot_parse`
  - `mot_result_free`
- Missing declarations were implemented:
  - `vm_step` in `src/runtime/vm.c`
  - `bytecode_deserialize` in `src/compiler/bytecode.c`
- Example origin server now has first-pass hardening:
  - bounded URL decoding
  - path traversal prevention for page/component/import paths
  - `/data` is POST RPC (`/data/{page}`) with `queryRef` + `signature` + bound params

Outstanding gaps are tracked in `IMPLEMENTATION_PLAN.md` (Priority 3 and 4).

Current implementation priority:
- Execute architecture track first (`Sprint 9` through `Sprint 15` in `IMPLEMENTATION_PLAN.md`).
- Defer non-architecture cleanup unless it blocks that track.

## Build and test

Core:
```bash
make clean
make test
```

Current suite count:
- Lexer: 9
- Parser: 19
- Analyzer: 17
- Compiler: 23
- Partial eval: 18
- VM: 37
- Codegen: 10
- API: 11
- CLI target: 1
- Debug metadata: 2
- Source map: 5
- Host contract: 3
- Transpiler: 2
- Integration (e2e): 45
- Total: ~198

Example origin server:
```bash
make -C example/origin
```

## Key files by responsibility

- Public API: `src/mot.h`, `src/mot.c`
- CLI: `src/cli/mot_cli.c` (`make cli`, output `build/mot`)
- Parser + AST: `src/parser/parser.c`, `src/parser/ast.h`
- Analyzer: `src/analyzer/analyzer.c`
- Compiler: `src/compiler/compiler.c`
- Bytecode format/serdes: `src/compiler/bytecode.h`, `src/compiler/bytecode.c`
- VM: `src/runtime/vm.h`, `src/runtime/vm.c`
- Source maps/debug mapping helpers: `src/debug/sourcemap.h`, `src/debug/sourcemap.c`
- CSS/JS extraction: `src/codegen/codegen.c`, `src/codegen/css.c`
- Demo origin server: `example/origin/server.c`

## Important behavioral notes

- Bytecode multi-byte encoding is little-endian.
- `mot_compile` allocates `bytecode`, `css`, `js`, and `errors` on heap. Always call `mot_result_free`.
- `mot_parse` uses caller-provided arena and optional preallocated error array (`MotErrorList`).
- `vm_step` now executes exactly one instruction and returns `VM_OK`/`VM_ERROR`.
- `match/case/default` now parses/analyzes/compiles/executes.
- Ternary expressions (`if cond then a else b`) are compiled and executed.
- `export` and `interface` parse as language constructs and are handled as compile-time metadata.
- `macro` declarations are supported with compile-time expansion, named params, and `<children>` substitution.
- Partial evaluation is now integrated in compiler expression emission under `compiler_set_partial_eval`.
- Compiler tests intentionally run baseline assertions with partial-eval disabled, plus dedicated PE-on regression cases.
- Bytecode `1.2` serializes SQL data requirements as `queryRef + signature + param bindings` without embedding raw SQL text.
- Bytecode loading is intentionally strict in development: only `1.2` is accepted (no backward compatibility path).
- Source-map foundation now exists in core C helpers (`src/debug/sourcemap.c`) with Base64-VLQ mappings and v3 JSON payload rendering.
- Compiler target API supports explicit `MOT_TARGET_BYTECODE|WASM|BOTH` via `mot_compile_with_options`.
- C VM now covers JS-interpreter opcode parity for `BC_AND`, `BC_OR`, `BC_CALL`, `BC_CALL_PIPE`, and `BC_CONCAT`.
- JS and C interpreters both now error on `BC_ITER_START` when the iterable is not an array (parity behavior).
- Call-frame cleanup on `BC_RETURN` now restores caller stack base, preventing stack growth across repeated component/function calls.
- `runtime-wasm` is being migrated to reuse shared core `src/runtime/vm.c` + `src/compiler/bytecode.c` (avoid divergent interpreter logic).
- `runtime-wasm` now supports async resume (`mot_render`/`mot_resume`) for `BC_FETCH_DATA` and `BC_COMPONENT_LOAD`.
- `runtime-wasm` now also exports `mot_render_component(func_idx, props_json, children_json)` for direct component-function execution.
- Edge wasm path runs `index.mot` end-to-end in `--vm wasm` mode via `example/e2e-wrangler.sh`.
- Debug e2e trace checks are available via `example/e2e-debug.sh --mode trace` (wrapper around wrangler e2e with debug assertions).
- Render trace schema drift is guarded by snapshot test:
  `example/edge/render-trace.snapshot.json` + `example/edge/test-render-trace.mjs`.
- WASM target smoke checks across fixture pages are available via `example/target-matrix.sh`.
- Multi-page parity fixtures are configured in `example/parity-pages.txt`.
- Optional e2e wrapper is available via `make test_wasm_parity` (set `MOT_E2E=1`).
- CI workflow now includes `core`, `edge-unit`, and `e2e-parity` jobs in `.github/workflows/ci.yml`.
- C-side data-contract fixtures run via `tests/host/test_contract.c` (`make test_host_contract`).
- Contract snapshot parity checks run in edge unit tests (`example/edge/test-contract-snapshot.mjs`).
- Browser runtime contract parity baseline is in `browser-runtime/src/runtime-contract.js` with
  test `browser-runtime/test-runtime-contract.mjs`.
- Browser runtime host adapter now exists in `browser-runtime/src/host.js` with tests in `browser-runtime/test-host.mjs`.
- Runtime-wasm host callback behavior is validated by `runtime-wasm/test-host-adapter.mjs`.
- Browser wasm bootstrap injection helper is in `example/edge/src/browser-wasm-inline.js`
  with focused test `example/edge/test-browser-wasm-inline.mjs`.
- CLI target checks run via `tests/cli/test_cli_target.sh` (`make test_cli`).
- Example origin now supports startup precompile caches (manifest-driven) for pages/components.
- Example edge can bundle precompiled system components and load them locally without origin component RPC.
- Edge debug metadata now includes a preliminary source-map payload (`bytecodeDebug.sourceMap`) derived from bytecode debug spans.
- Debug trace payload now includes `componentSourceMaps` for loaded components when maps are available.
- Debug trace payload now also includes `executionPaths` to make mixed runtime paths inspectable.
- Precompile step now also emits component map artifacts (`example/edge/debug-maps/*.map.json`) and
  generated map index (`example/edge/src/system-components.maps.generated.js`) for embedded components.
- Runtime-contract helpers now also standardize component path handling:
  `normalizeComponentPath`, `normalizeComponentRef`, `componentRouteName`, `buildComponentRpcUrl`.
- Edge worker and wasm adapter component loads now use normalized component refs/paths from contract helpers.
- Origin now serves browser runtime wasm via `GET /runtime/wasm`.
- Browser-side wasm execution can be enabled globally with worker var `MOT_BROWSER_WASM=1`
  or per request via `?browser_wasm=1`.
- Compiler now emits synthetic reactive markers:
  - `@bind:<path>` for directly bindable outputs (`ident` and member chains like `user.name`)
  - `@set:<path>` for compile-time observed mutable targets in `<set ...>`
- Compiler also emits explicit reactive plan markers:
  - `@plan:<setPath>|<bindPath>` for mutation-to-binding mappings known at compile time.
- Browser wasm runtime now parses dependency markers directly from serialized bytecode headers
  to build reactive indexes (`planSetToBind`, `planAttrBySet`, `setToBind`, `setToAttr`, `rootToBind`, `rootToAttr`)
  used by update scheduling.
- Browser wasm bootstrap now exposes helper APIs:
  - `window.motSetVar(name, value)` for direct bound text updates
  - `window.motSignal(name, initial)` for primitive reactive updates
  - `window.motCreateState(name, object)` for proxy-based nested object updates
  - `window.motFlushReactivity()` to force-drain queued reactive updates in development/tests
  - `window.motListBindings()` / `window.motRefreshBindings()` for debug/manual control
- Browser wasm reactive updates are now microtask-batched to coalesce multiple same-tick mutations.
- Browser wasm bytecode fetches are cached in-memory per route (`pageBytecodeCache` / `componentBytecodeCache`)
  so repeated component loads avoid refetch+reparse churn during a session.
- Compiler now emits expression-level reactive markers for supported expression ASTs:
  - output expressions: `@exprbind:<exprId>|<program>` + `@exprdep:<exprId>|<depPath>`
  - attribute expressions: `@exprattr:<nodeId>|<attr>|<exprId>|<program>` + `@exprdep:<exprId>|<depPath>`
  - mutation plans: `@planexpr:<setPath>|<exprId>`
- Browser wasm runtime evaluates expression programs from metadata and applies targeted updates to:
  - expression output spans (`data-mot-expr`)
  - expression-backed attributes on `data-mot-node` targets
- Edge now generates a per-page reactive wasm companion module from compiler markers
  (`@exprbind` / `@exprattr`) and embeds it in browser bootstrap payload.
- Browser runtime loads this page-specific wasm module and evaluates compiled numeric expression updates
  via exported wasm functions, with JS evaluator fallback for unsupported expression forms.
- Browser runtime no longer calls origin directly from the page; it uses edge same-origin proxy endpoints:
  `/_mot/runtime/wasm`, `/_mot/page/{name}`, `/_mot/component/{name}`, `/_mot/data/{page}`.
- Example runner now defaults browser bootstrap on (`MOT_BROWSER_WASM=1`) and example pages use declarative
  reactive action attributes (`data-mot-inc`, `data-mot-set-path`, `data-mot-set-prefix`) instead of inline page JS handlers.
- Edge dependency collection intentionally filters synthetic dependency markers starting with `@`.

## Known traps

- `PROJECT_STRUCTURE.md` is now aligned to current code; use `IMPLEMENTATION_PLAN.md` for sprint-level progress and remaining gaps.
- SQL `:param` syntax is parsed and compiled into data requirement metadata.
- Deserialized edge/runtime modules do not include SQL text (`DataRequirement.query_len == 0`); origin recompilation is the source of executable SQL.
- Any non-`1.2` bytecode payload is rejected by deserializers/interpreters (intentional breaking policy during development).
- `export`/`interface` do not yet emit serialized module metadata (currently analysis/compiler-only behavior).
- Macro expansion currently expects valid XML call form (`<macro-name ... />` when no children).
- Macro declarations are currently resolved in source order (declare before use).
- Disassembler operand decoding has an endian display bug (debug output can look wrong even when execution works).
- WASM async bridge currently supports one in-flight async request at a time (sequential VM execution model).
- Edge runtime now executes WASM-only for pages and dynamic components (no JS interpreter fallback path in worker execution).
- For freestanding wasm, integer/number formatting currently uses local minimal conversion helpers; `stb_sprintf` is a planned follow-up for parity.
- Local `wasm-ld` in this toolchain does not support `--source-map`; true wasm-offset source maps remain pending.
- Core compiler `--target wasm|both` currently fails fast with a clear error until wasm emit integration lands.
- Browser wasm mode now applies in-place DOM morphing after client render (no `document.write` full replacement),
  and path-based reactive updates are microtask-coalesced.
- Expression-level reactive updates now support literals, identifiers/member paths, index access,
  unary/binary ops, and ternary for targeted text/attribute patching.
  Calls/pipes/object/array expressions are still not compiled into reactive patch programs.
- Current reactive helpers are path-based and now cover:
  - text bindings (`@bind:`)
  - bindable attribute expressions via stable node ids (`@bindattr:` / `@planattr:`)
  - supported expression-level patch programs (`@exprbind`, `@exprattr`, `@exprdep`, `@planexpr`)
- Origin startup precompile list is controlled by `example/precompile-pages.txt`; avoid adding pages that currently hit parser recovery hangs until fixed.
- Edge system-component bundle is generated from `example/system-components.txt` into `example/edge/src/system-components.generated.js`.
- Component call compilation must capture children before props and reload children for call order.
  This avoids local-slot misalignment when child blocks declare locals (for example SQL-backed `<let>` inside layout slots).
- WASM host bridge must treat imported `i32` metadata fields as unsigned (`>>> 0`) for `queryRef`/`signature`.
  Signed interpretation can break origin-side `u32` validation in `/data` RPC.

## Working conventions

- Prefer `rg` for file/text discovery.
- Preserve little-endian assumptions in bytecode read/write paths.
- Add focused regression tests with each fix.
- For security-sensitive demo server changes, keep behavior explicit and fail-closed.
