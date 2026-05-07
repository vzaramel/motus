# Debug Tracking Architecture

This document explains how debug information is tracked end-to-end in the current implementation, including:

- Origin component precompile cache
- Bytecode/VM execution
- WASM runtime execution on edge
- Browser debug view attribution (hover/inspect)

## 1. Compiler Layer: What Debug Data Is Produced

The compiler records debug metadata in bytecode:

- `FunctionDebugRef` per compiled function/component (`name`, `source_path`)
  - Defined in `src/compiler/bytecode.h`
  - Set during component compilation via `bytecode_set_function_debug(...)` in `src/compiler/compiler.c`
- Line-span info per chunk (main + function chunks)
- Query debug info (`queryRef`, `signature`, line/column, binding name)
- Data requirements (`queryRef`, `signature`, parameter names/slots)

All of this is serialized in an optional debug trailer:

- Written in `bytecode_serialize_ex(..., include_debug=true)` in `src/compiler/bytecode.c`
- Trailer shape:
  - `BYTECODE_DEBUG_MAGIC`
  - spans
  - query entries
  - function debug refs

## 2. Origin Layer: Source Provenance Across Imports

The origin compiler stage resolves static imports and preserves source provenance:

- Top-level components in a file are tagged with that file source path
- Nested imported `defcomp` source paths are preserved (not overwritten by parent importer)

This logic is in `example/origin/server.c` (`tag_top_level_defcomp_sources`, `resolve_imports`).

Why this matters:

- Without this, nested components (e.g. `Header`, `Footer`, `ProductCard`) can collapse to the wrong source file in debug view.

## 3. Precompiled System Components (Origin Cache)

Precompilation script:

- `example/precompile-system-components.mjs`
- Compiles listed components via origin `/component/...`
- Emits:
  - `example/edge/src/system-components.generated.js` (bytecode bundle artifact)
  - `example/edge/src/system-components.maps.generated.js` (source-map objects)

Important:

- The active edge runtime path does not embed/patch component artifacts into `mot-runtime.wasm`.
- Components are resolved via origin `/component/...` and cached at edge once loaded.

## 4. Edge Runtime: VM Mode + Data/Component Trace

Edge worker:

- Forces wasm VM mode in `example/edge/src/worker.js` (`resolveVmMode`)
- Fetches page bytecode from origin
- Extracts debug metadata client-side from raw bytes using:
  - `extractBytecodeDebugMetadata(...)` in `example/edge/src/bytecode-debug.js`

For each request, edge tracks:

- Execution spans (fetch/parse/run timing)
- Data RPCs (`queryRef`, `signature`, duration, status, row count)
- Component load source (`cache`, `origin`, `linked_bundle`)
- Component render info (`renderEngine`, `loadSource`, `artifactSource`, `componentOpcode`, duration)
- Per-component source maps

Collected in `RenderTrace` (`example/edge/src/render-trace.js`) and injected into HTML as:

- `<script id="mot-trace-data" type="application/json">...</script>`

## 5. Runtime Attribution Signals from VM

Core C VM emits static-component boundary hooks:

- Component start/end at `BC_COMPONENT_START` and return
- Slot default start/end at `BC_SLOT_DEFAULT`

Hook plumbing:

- Hook types in `src/runtime/vm.h`
- Triggered in `src/runtime/vm.c`
- Forwarded by wasm adapter in `runtime-wasm/src/wasm_vm.c`

When debug mode is enabled, wasm adapter emits inline marker comments into HTML stream:

- `<!--motc:{funcIdx}:start-->`
- `<!--motc:{funcIdx}:end-->`
- `<!--motslot:{funcIdx}:start-->`
- `<!--motslot:{funcIdx}:end-->`

These markers are emitted alongside VM output so slot-capture boundaries are preserved.

## 6. Browser Debug View: How Attribution Is Computed

Devtools bootstrap script:

- `example/edge/src/devtools-inline.js`

Attribution algorithm (current):

1. Read `trace.bytecodeDebug.functionComponents`
2. Build map `funcIdx -> {source, component, render/load/artifact}`
3. On hover target:
   - Replay marker stream from document start up to target node
   - Maintain:
     - `componentStack`
     - `slotSuspendedStack`
   - Rules:
     - `motc:start` -> push component
     - `motc:end` -> pop component
     - `motslot:start` -> temporarily suspend active component if same function
     - `motslot:end` -> restore suspended component
4. Active component at target gives attribution.
5. If none active -> fallback to page source.

This is the mechanism intended to support higher-order components generically:

- Wrapper layout markup remains attributed to layout component
- Only `<children>` content is attributed to caller context unless nested component frames override it

## 7. Runtime Path

Edge rendering uses the wasm VM runtime path.

## 8. Known Fragility Points

Attribution can drift if any of these break:

- Incorrect `source_path` propagation during import flattening
- Missing function debug refs in bytecode trailer
- Marker ordering/stack mismatch across component + slot boundaries
- Browser DOM normalization changing traversal assumptions

Most recent fixes focused on:

- Preserving nested component source paths in origin import resolution
- Using marker-driven attribution with explicit slot suspension/resume semantics

## 9. What This Debug System Currently Answers

From the devtools panel + inspect mode you can identify:

- Which `.mot` source contributed a hovered region
- Whether component artifact came from cache or origin
- Whether render path used wasm VM
- Which data queries executed and how long they took
- Which static component functions exist in bytecode debug metadata

## 10. VM Source-Step Debugging (Chrome DevTools)

The wasm runtime now exposes per-instruction debug-step callbacks mapped to bytecode debug spans:

- Runtime export: `mot_set_debug_step_mode(1|0)`
- Host import: `host_debug_step(chunkKind, chunkIndex, pc, opcode, line, column, sourcePath, sourceLen)`

What this enables:

- While running the VM interpreter, host can pause at `.mot`-mapped locations instead of stepping raw VM C/WASM opcodes.
- `sourcePath` comes from function debug metadata (`FunctionDebugRef`) for function/component chunks.
- `line`/`column` comes from bytecode debug spans for the current instruction PC.

Current host API (`runtime-wasm/src/host.js`):

- `createHost({ onDebugStep, pauseOnDebugStep })`
- `runtime.setDebugStepMode(true)` to enable wasm step events
- `runtime.setPauseOnDebugStep(true)` to auto-pause on each step

Important limitation:

- This is source-attributed stepping driven by runtime callbacks.
- It is not native DWARF remapping of interpreter machine code to `.mot`; Chrome cannot do that automatically for dynamic bytecode execution.
