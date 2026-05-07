# Remediation Plan

This plan tracks implementation work from the review, prioritized by risk.

## Immediate Execution Order (Current Priority)

Architecture work is now the top priority and must be implemented before additional feature work.
Effective immediately, execution order is:

1. Sprint 9 - Canonical Runtime Contract
2. Sprint 10 - Debug Metadata + Render Trace
3. Sprint 11 - Source Maps for WASM
4. Sprint 12 - First-Class Target Matrix
5. Sprint 13 - Mixed Edge Execution
6. Sprint 14 - Browser/Editor WASM Runtime + DevTools
7. Sprint 15 - Granular Reactive Updates

Policy:
- No new non-architecture feature work should start until the above sequence is complete or explicitly waived.
- Performance cleanup remains important but is secondary to locking architecture correctly.

## Architecture Acceptance Checklist

This section is the source of truth for validating the target system design.
Items here are pass/fail requirements, not optional ideas.

### A. Dual Targets Are First-Class

- [ ] Compiler supports `--target bytecode|wasm|both` as first-class outputs (not a special-case path).
- [ ] Analyzer/dependency tracking/debug metadata flows support both targets.
- [ ] CI includes target-matrix coverage (`bytecode`, `wasm`, `both`) for representative pages/components.

### B. Mixed Execution On Edge

- [ ] Precompiled/system modules can execute as pre-deployed WASM in edge runtime.
- [ ] User-defined/on-demand modules can execute as bytecode on edge VM.
- [ ] Mixed renders are supported in a single request (WASM app parts + VM dynamic parts).
- [ ] VM can invoke pre-deployed WASM components through a unified component loader contract.

### C. Browser/Editor Runtime

- [ ] Browser/editor runtime executes native WASM modules only (no shipped VM/interpreter).
- [ ] Initial HTML is rendered at edge, and companion WASM modules are delivered for reactive updates.
- [ ] On-demand user content can be transpiled to WASM and executed in browser/editor runtime.
- [x] Browser/editor host uses the same logical component/data resolution contract as edge runtime.

### D. Shared RPC Contract Across Runtimes

- [x] Data RPC contract is canonical across edge VM, edge WASM, and browser/editor WASM:
      `POST /data/{page}` with `{ queryRef, signature, params, single }`.
- [x] Query validation uses `queryRef + signature` in all runtimes.
- [x] Component lookup path namespace is consistent across edge and browser/editor hosts.

### E. Development Debug Experience

- [ ] Development responses include render trace metadata sufficient to explain:
      module loads/import graph, data fetches, render source mapping, and timing per stage/span.
- [ ] A development sidebar/DevTools view can inspect rendered regions and map them to source/module origin.
- [ ] Debug traces include timing for compile/fetch/render/component-load phases.
- [ ] Debug mode includes source mapping artifacts enabling browser debugging of generated WASM back to `.mot`.

### F. Reactive Vision Alignment

- [ ] Browser WASM modules can perform granular component-level updates without full page rerender.
- [ ] Dependency tracking supports selective invalidation for reactive updates.
- [ ] Reactive data/module fetches reuse the same host/API semantics as edge execution.

## Priority 0: Safety and correctness blockers

- [x] Fix VM builtin argument handling overflow in `src/runtime/vm.c`.
- [x] Add regression test that previously triggered crash due to large builtin arg count.
- [x] Fix `if/elsif/else` codegen branch patching and chain traversal in `src/compiler/compiler.c`.
- [x] Align analyzer branch traversal with parser AST shape for `elsif` chains in `src/analyzer/analyzer.c`.
- [x] Add runtime regression tests for single and multiple `elsif` branches.

## Priority 1: API completeness

- [x] Implement public API declared in `src/mot.h` (`mot_compile`, `mot_result_free`, `mot_parse`) or remove declarations until implemented.
- [x] Implement `vm_step` declared in `src/runtime/vm.h` or remove declaration.
- [x] Implement `bytecode_deserialize` declared in `src/compiler/bytecode.h` or remove declaration.

## Priority 2: Security hardening (example server)

- [x] Add path normalization and traversal prevention for `/page` and `/component` routes.
- [x] Bound all URL decoding writes (`url_decode`) and route buffers.
- [x] Remove raw SQL execution from URL path in `/data` or add strict allowlist/parameterization and auth gate.
- [x] Remove raw SQL text from serialized edge bytecode data requirements; keep `queryRef + signature + param` metadata only.

## Priority 3: Feature parity and design alignment

- [x] Implement/gate `export` and `interface` so they parse/analyze/compile predictably (metadata, not runtime output).
- [x] Implement macro declarations and compile-time expansion with params and `<children>` support.
- [x] Implement `match/case/default` end-to-end (parser/analyzer/compiler/runtime) with regression tests.
- [x] Implement ternary expression (`if cond then a else b`) compilation/runtime support with tests.
- [x] Add parser support for SQL parameters (`:param`) and tests.
- [x] Integrate partial evaluation pass into compiler pipeline under `partial_eval` flag.
- [x] Reconcile docs with implementation status in `PROJECT_STRUCTURE.md`.

## Priority 4: Performance-focused cleanup (Deferred until architecture track is stable)

- [ ] Replace runtime builtin string-dispatch with index/function-table dispatch.
- [ ] Replace linear object lookup with hash-based lookup path.
- [ ] Add capacity checks/growth for CSS scoping output buffer.
- [ ] Align opcode support and behavior across C VM, runtime-wasm, and edge JS interpreter.
- [ ] Add bytecode-level partial evaluation pass (post-compile IR optimization) so precompiled page/component artifacts can be specialized without re-running full AST compilation.

## Priority 5: WASM Core Reuse

- [x] Stop adding behavior to the standalone WASM interpreter implementation.
- [x] Switch `runtime-wasm` build to compile shared `src/runtime/vm.c` + `src/compiler/bytecode.c`.
- [x] Add freestanding wasm arena/runtime adapter with exported allocator init path.
- [x] Implement async data resume path (`mot_resume`) so wasm can consume fetch results.
- [x] Add wasm-vs-js parity test fixtures and CI checks.
- [ ] Optional: adopt `stb_sprintf` for freestanding formatting parity once core path is stable.

## Sprint 1 (Done)

1. Fix VM arg overflow + regression test.
2. Fix `elsif` execution correctness + regression tests.
3. Run full test suite.

## Sprint 2 (Done)

1. Implement missing public API and declared runtime/bytecode functions.
2. Add API tests (`tests/api/test_api.c`) and VM step regression.
3. Harden example origin server path/query handling.

## Sprint 3 (Done)

1. Add `CODEX.md` as project upfront context for future runs.
2. Sync project status and navigation notes into `CODEX.md`.
3. Keep remediation roadmap as source of truth in this plan file.

## Sprint 4 (Done)

1. Wire partial evaluation into `compiler.c` expression emission.
2. Synchronize PE scope/binding behavior with lexical scopes and mutations.
3. Add compiler regression tests for PE enabled/disabled behavior.

## Sprint 5 (Done)

1. Bump bytecode format to `1.2` and strip serialized SQL text from data requirements.
2. Drop backward compatibility paths and enforce strict `1.2` bytecode loading during development.
3. Update edge JS interpreter parser and API regression tests for query-reference-only RPC metadata.

## Sprint 6 (Done)

1. Audit C VM vs JS interpreter opcode parity and close missing opcode handlers in C.
2. Add C VM support/tests for `BC_AND`, `BC_OR`, `BC_CONCAT`, `BC_CALL`, and `BC_CALL_PIPE`.
3. Fix `BC_RETURN` stack-base restoration to avoid stack growth across repeated component/function calls.

## Sprint 7 (Done)

1. Add shared-VM await/resume flow for wasm (`VM_AWAIT_DATA`, `mot_resume`).
2. Implement wasm async host bridge for query-ref fetch RPC and dynamic component load callbacks.
3. Validate end-to-end with wrangler on complex page (`index.mot`) in forced `--vm wasm` mode.

## Sprint 8 (Done)

1. Fix component call compilation order so slot children locals are not misaligned by transient props values.
2. Normalize wasm-imported `i32` request metadata to unsigned in JS bridge (`queryRef`/`signature`).
3. Align JS interpreter `BC_ITER_START` error semantics with C VM and re-validate `layout`/`index` in wasm+js modes.

## Sprint 9 (Done) - Canonical Runtime Contract

Checklist coverage: `A`, `D`

Progress update:
- [x] Added shared runtime-contract helpers for edge (`example/edge/src/runtime-contract.js`).
- [x] Added runtime-wasm host contract helpers (`runtime-wasm/src/runtime-contract.js`).
- [x] Added contract tests for both adapters (`example/edge/test-host-contract.mjs`, `runtime-wasm/test-host-contract.mjs`).
- [x] Added contract snapshot parity checks (`example/edge/test-contract-snapshot.mjs`, `example/edge/contract-snapshot.json`).
- [x] Added C-side contract fixtures for data RPC and component path namespace (`tests/host/test_contract.c`, `make test_host_contract`).
- [x] Added browser runtime contract module + parity tests (`browser-runtime/src/runtime-contract.js`, `browser-runtime/test-runtime-contract.mjs`).
- [x] Added shared component-path normalization + RPC URL helpers (`normalizeComponentRef`, `buildComponentRpcUrl`) and wired them into edge/wasm adapters.
- [x] Added browser host adapter with contract-based data/component RPC (`browser-runtime/src/host.js`, `browser-runtime/test-host.mjs`).
- [x] Added runtime-wasm host adapter callback tests for normalized fetch/component requests (`runtime-wasm/test-host-adapter.mjs`).

1. Define canonical host/runtime contract shared by edge VM, edge WASM, and browser/editor WASM.
2. Implement typed adapters for data RPC and component lookup path resolution.
3. Remove divergent payload shapes and path handling from runtime-specific code.

Validation tests:
- Add contract fixture tests: `tests/host/test_contract.c`
- Add JS parity tests for host adapters: `example/edge/test-host-contract.mjs`
- Add CI step to assert contract snapshots match across VM/WASM hosts.

## Sprint 10 (Planned) - Debug Metadata + Render Trace

Checklist coverage: `E`

Progress update:
- [x] Added debug e2e wrapper `example/e2e-debug.sh --mode trace`.
- [x] Debug e2e assertions now validate trace script, devtools bootstrap, page `sourceMap`, and `componentSourceMaps` payloads.
- [x] Added dedicated C debug-metadata unit tests (`tests/debug/test_debug_metadata.c`).
- [x] Added render trace schema snapshot regression (`example/edge/render-trace.snapshot.json`, `example/edge/test-render-trace.mjs`).

1. Add debug metadata section to bytecode with source spans, query refs, and component refs.
2. Instrument render path with timing spans (compile/fetch/render/component load).
3. Embed trace payload in development responses and wire basic sidebar bootstrap.

Validation tests:
- Add bytecode debug metadata tests: `tests/debug/test_debug_metadata.c`
- Add e2e assertion that dev responses include trace payload + source attributes:
  `example/e2e-debug.sh --mode trace`
- Add regression snapshot for trace schema.

## Sprint 11 (Planned) - Source Maps for WASM

Checklist coverage: `E`

Progress update:
- [x] Added source map VLQ/mappings/json foundation in `src/debug/sourcemap.c`.
- [x] Added focused test coverage in `tests/debug/test_sourcemap.c` and `make test_sourcemap`.
- [x] Edge debug trace now emits a preliminary source-map payload derived from bytecode spans for DevTools inspection.
- [x] Added per-module map artifacts for precompiled system components:
      `example/edge/debug-maps/*.map.json` + `example/edge/src/system-components.maps.generated.js`.
- [ ] Integrate mappings with generated WASM instruction offsets (current local `wasm-ld` lacks `--source-map` support).

1. Implement source map generation pipeline for generated WASM modules.
2. Map `.mot` source spans to wasm offsets and export VLQ source maps.
3. Verify browser debugger integration for mapped source locations.

Validation tests:
- Add source map encoder tests: `tests/debug/test_sourcemap.c`
- Add wasm mapping roundtrip tests: `tests/codegen/test_wasm_sourcemap.c`
- Add browser integration test that maps a known runtime frame to `.mot` source line.

## Sprint 12 (Planned) - First-Class Target Matrix

Checklist coverage: `A`

Progress update:
- [x] Added target/VM matrix runner `example/target-matrix.sh` for `js`, `wasm`, and `auto` modes.
- [x] Added normalized HTML parity assertion between `js` and `wasm` outputs.
- [x] Added fixture-driven matrix runs (`example/parity-pages.txt` + `--pages-file`).
- [x] Added opt-in parity test target (`make test_wasm_parity`, `MOT_E2E=1`).
- [x] Added CI matrix/parity automation (`.github/workflows/ci.yml`, job `e2e-parity`).
- [x] Added compiler API target selection (`mot_compile_with_options`) with explicit `bytecode|wasm|both`.
- [x] Added CLI target entrypoint (`build/mot --target ...`) and automated CLI target tests.
- [ ] Implement core wasm emit path so `--target wasm|both` produces artifacts instead of fail-fast errors.

1. Implement `--target bytecode|wasm|both` end-to-end in compiler/tooling.
2. Ensure shared analyzer/dependency/debug pipeline works for each target.
3. Publish target matrix in CI for representative fixtures.

Validation tests:
- Add CLI tests for output artifacts per target mode.
- Add matrix job verifying compile success + smoke execution for each target.
- Add fixture parity test: same page renders equivalent HTML across target modes.

## Sprint 13 (Planned) - Mixed Edge Execution

Checklist coverage: `B`, `D`

Progress update:
- [x] Added execution-path trace markers for VM/component runtime paths (`executionPaths` in `RenderTrace`).
- [x] Added debug e2e assertion that trace payload includes `executionPaths`.
- [x] Added wasm runtime component render export (`mot_render_component`) to execute function chunks with props/children JSON.
- [x] Edge wasm path now attempts component rendering via wasm first (`component:wasm`) with safe JS fallback on failure.

1. Implement edge component loader that can resolve predeployed WASM and origin bytecode.
2. Support VM invoking predeployed WASM components during mixed render.
3. Keep output streaming and dependency tracking coherent across mixed execution.

Validation tests:
- Add mixed runtime e2e page fixture (app shell WASM + user bytecode content).
- Add wrangler e2e: `example/e2e-wrangler.sh --vm auto --page mixed`
- Add assertion that both execution paths are exercised in one request trace.

## Sprint 14 (Planned) - Browser/Editor WASM Runtime + DevTools

Checklist coverage: `C`, `E`

Progress update:
- [x] Added browser-side contract helpers and host adapter for data/component RPC parity:
      `browser-runtime/src/runtime-contract.js`, `browser-runtime/src/host.js`.
- [x] Added browser host contract tests:
      `browser-runtime/test-runtime-contract.mjs`, `browser-runtime/test-host.mjs`.
- [x] Added opt-in browser wasm bootstrap path on edge:
      `example/edge/src/browser-wasm-inline.js`, enabled via `MOT_BROWSER_WASM=1` or `?browser_wasm=1`.
- [x] Added origin endpoint to serve runtime wasm to browser:
      `GET /runtime/wasm` in `example/origin/server.c`.
- [x] Added same-origin edge proxy endpoints used by browser runtime:
      `/_mot/runtime/wasm`, `/_mot/page/{name}`, `/_mot/component/{name}`, `/_mot/data/{page}`.
- [x] Added browser bootstrap unit test and e2e assertion wiring:
      `example/edge/test-browser-wasm-inline.mjs`, `example/e2e-wrangler.sh --browser-wasm`.
- [x] Browser bootstrap now applies wasm-rendered HTML with in-place DOM morphing (no `document.write` full replacement).
- [x] Example runner now defaults browser runtime bootstrap on (`MOT_BROWSER_WASM=1`) for local validation.

1. Deliver browser/editor runtime that executes WASM only (no VM bundle in client payload).
2. Implement sidebar inspect mode mapping DOM regions to source/module/query/timing.
3. Ensure browser/editor host uses same logical data/module contract as edge.

Validation tests:
- Add browser runtime tests: `tests/browser-runtime/test_runtime.js`
- Add devtools UI tests: `tests/browser-runtime/test_devtools.js`
- Add payload audit test confirming VM/interpreter code is absent from browser bundle.

## Sprint 15 (Planned) - Granular Reactive Updates

Checklist coverage: `F`, `C`

Progress update:
- [x] Compiler emits direct reactive bind/set markers:
      `@bind:<path>`, `@set:<path>`.
- [x] Compiler emits explicit mutation-to-binding plans:
      `@plan:<set>|<bind>`.
- [x] Compiler emits attribute reactive markers with stable node ids:
      `data-mot-node`, `@bindattr:<node>|<attr>|<bind>`, `@planattr:<set>|<node>|<attr>|<bind>`.
- [x] Browser wasm runtime parses dependency markers from bytecode headers and builds indexes:
      `planSetToBind`, `planAttrBySet`, `setToBind`, `setToAttr`, `rootToBind`, `rootToAttr`.
- [x] Browser runtime updates only matched text/attribute bindings via path-based invalidation APIs:
      `window.motSetVar`, `window.motSignal`, `window.motCreateState`.
- [x] Compiler/runtime now support expression-level targeted patch metadata:
      `@exprbind`, `@exprattr`, `@exprdep`, `@planexpr`.
- [x] Browser runtime evaluates expression patch programs and updates only affected expression outputs/attrs.
- [x] Edge builds and ships per-page reactive wasm companion modules from compiler metadata (`@expr*` markers).
- [x] Browser runtime executes compiled reactive wasm exports for numeric expression updates (JS fallback retained).
- [x] Example pages now use declarative runtime action attributes (`data-mot-inc`, `data-mot-set-path`, `data-mot-set-prefix`)
      instead of inline page-level reactive JS wiring.
- [x] Reactive updates are microtask-coalesced in browser runtime to reduce redundant DOM writes.
- [x] Browser runtime now caches page/component bytecode fetches in-session to reduce repeat decode overhead.

1. Expand wasm expression compiler coverage for remaining AST kinds and non-numeric value semantics.
2. Reuse same data/module host semantics for reactive fetches in browser follow-up renders.
3. Add component-subtree invalidation boundaries (avoid whole-document morphing fallback paths).

Validation tests:
- Add reactive invalidation tests: `tests/browser-runtime/test_reactive.js`
- Add e2e test with controlled data mutation verifying only affected components rerender.
- Add trace assertion: changed dependency path updates subset of component tree only.
