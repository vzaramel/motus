# Motus Compiler - Claude Code Guide

## Project Overview

C99 library implementing "Motus" - a compiled XML-based language for building HTML pages with:
- XML-based syntax (superset of HTML), NO namespace prefixes
- Position-based syntax: `<let name value>` not `<y:let name="value">`
- Partial evaluation (static→compile-time, dynamic→bytecode holes)
- SQL as first-class citizen in declarations
- Fine-grained dependency tracking (e.g., `product.name` not `product`)
- Streamable bytecode for WASM edge execution
- Component system with props, slots, and scoped CSS
- Reactive state with `var`/`set` and reactive binding markers
- Pipe syntax for chaining transformations

## Environment Setup

**IMPORTANT**: Activate mise before using node/npm:
```bash
eval "$(mise activate bash)"  # or zsh
```

## Build Commands

```bash
make          # Build library (libmot.a)
make test     # Run all tests (~198 total across 14 suites)
make cli      # Build CLI tool (build/mot)
make transpiler # Build WASM transpiler CLI
make clean    # Full rebuild
```

## Project Structure

```
src/                    # Core compiler library (C99, ~18,500 LOC)
  mot.h / mot.c        # Public API
  lexer/               # Multi-mode lexer (XML/EXPR/SQL/STYLE/SCRIPT)
  parser/              # Recursive descent parser + AST definitions
  analyzer/            # Scope/types/dependencies/semantic analysis
  compiler/            # Bytecode emission + partial evaluation
  runtime/             # Stack-based bytecode interpreter (C)
  codegen/             # CSS/JS extraction with component scoping
  debug/               # Source map generation (VLQ Base64 v3)
  linker/              # Component reference resolution
  transpiler/          # Bytecode→WASM compilation
  cli/                 # CLI tools
  util/                # Arena allocator, hash tables, vectors, strings

runtime-wasm/          # WASM bytecode interpreter
  src/wasm_vm.c        # Freestanding WASM adapter (reuses shared C VM)
  src/host.h           # Host callback interface
  src/host.js          # JavaScript host adapter
  src/stream.c         # Buffered streaming output
  src/freestanding/    # Minimal libc replacements

example/               # Edge streaming demonstration
  origin/server.c      # HTTP server compiling .mot on-demand
  edge/src/            # Cloudflare Worker for edge rendering
    worker.js          # Edge worker entry point
    wasm-vm.js         # WASM VM bridge
  content/
    pages/             # Example .mot pages
    components/        # Example .mot components
  run.sh               # Start both servers (origin:8080, edge:8787)

tests/                 # ~198 tests across 14 suites
  lexer/               # 9 tests
  parser/              # 19 tests
  analyzer/            # 17 tests
  compiler/            # 23 tests (+ 18 partial eval)
  runtime/             # 37 tests
  codegen/             # 10 tests
  api/                 # 11 tests
  integration/         # 45 end-to-end tests
  debug/               # 7 tests (metadata + sourcemap)
  host/                # 5 tests (contract + transpiler)
  cli/                 # Shell script tests
  parity/              # WASM/JS parity tests (e2e, opt-in)

docs/                  # Documentation and GitHub Pages site
```

## Running the Example

```bash
cd example
eval "$(mise activate bash)"   # Activate mise for node/npm
./run.sh                       # Starts origin:8080 and edge:8787
```

## Key Design Decisions

- `let` = scoped bindings (children only), `var` = reactive state (component-scoped)
- Pipe syntax: `<output value | uppercase | trim>`
- SQL embedded: `<let products select * from products where active>`
- Component def: `<defcomp Name | prop1: type = default |>body</defcomp>`
- Let bindings create scope - variable visible only inside body
- SQL keywords (`count`, `sum`, etc.) are reserved - use alternatives like `counter`
- Comparison operators use keywords (`lt`, `gt`, `eq`, `neq`, `lte`, `gte`) to avoid XML conflicts
- Bytecode serializes SQL as `queryRef + signature` (no raw SQL text in bytecode)
- Reactive binding markers (`@bind`, `@set`, `@exprbind`) enable browser-side targeted DOM updates

## Compilation Pipeline

```
Source (.mot) → Lexer (multi-mode) → Parser (recursive descent)
             → AST (30+ node types)
             → Analyzer (scopes/types/deps)
             → Compiler (partial eval + bytecode emission)
             → BytecodeModule (v1.2, little-endian)
             → [Optional: Transpiler → WASM]
             → VM (stack-based interpreter)
             → Streaming HTML Output

             + CSS extraction (component-scoped)
             + JS extraction
             + Source map generation
```

## Bytecode Format

- All multi-byte values are **little-endian** (u16, u32, i16)
- Magic: `0x00544F4D` ("MOT\0"), Version: 1.2
- Opcodes use `BC_*` prefix (bytecode.h), AST operators use `OP_*` prefix (ast.h)
- 54 opcodes covering: stack ops, variables, literals, arithmetic, comparison, logic,
  control flow, iteration, streaming output, data fetching, functions, components,
  dependency tracking, collections

### Bytecode Sections
1. Header (magic, version, flags)
2. Section counts (const, string, dataReq, dep, builtin, compRef, func)
3. Constants pool
4. Strings table (interned, deduplicated)
5. Data requests (SQL queries with queryRef/signature)
6. Dependencies (fine-grained paths)
7. Builtins references
8. Component references
9. Main chunk (length + code)
10. Function chunks (one per component/macro)
11. Optional debug trailer (spans + query metadata + function refs)

## Code Patterns

### Compiler emits bytecode
```c
switch (node->data.binary.op) {  // Compare against AST OpType
    case OP_ADD: emit_byte(c, BC_ADD); break;  // Emit BC_* opcode
}
```

### For-loop iterator slot reservation
```c
emit_byte(c, BC_ITER_START);
c->scope->local_count++;  // Reserve slot for iterator
// ... loop body ...
c->scope->local_count--;  // Free iterator slot
```

### Reactive State Pattern
```xml
<var counter 0 />            <!-- Declares and initializes -->
<set counter counter + 1 />  <!-- Updates the variable -->
```

### Component Call Pattern
BC_COMPONENT_START pops props and children from stack, calls function:
1. Read u16 function index
2. Pop children value
3. Pop props value
4. Push call frame (return addr, code bounds)
5. Jump to function start
6. Push props as slot 0, children as slot 1

### Tag/Attribute Emission
- BC_EMIT_TAG_OPEN emits `<tag` (no closing `>`)
- BC_EMIT_TAG_END emits `>` to close opening tag
- Attributes emitted between TAG_OPEN and TAG_END
- Lexer has `in_element_content` flag to preserve whitespace in text content

## Public API

```c
/* Compile source to bytecode + CSS + JS */
MotCompileResult mot_compile(const char *source, size_t source_len);

/* Compile with options (target, partial_eval, include_debug) */
MotCompileResult mot_compile_with_options(const char *source, size_t source_len,
                                          const MotCompileOptions *options);

/* Parse only (for tooling) */
AstNode *mot_parse(const char *source, size_t source_len,
                   Arena *arena, MotErrorList *errors);

/* Free compilation result */
void mot_result_free(MotCompileResult *result);
```

## Known Limitations

- SQL keywords (`count`, `sum`, `avg`, `min`, `max`, etc.) are reserved in all modes
- Use alternative names like `counter` instead of `count` for variables
- Comparison operators use keyword syntax (`lt`, `gt`, `eq`) to avoid XML conflicts
- `--target wasm|both` in core compiler returns not-implemented; use runtime-wasm build pipeline
- Reactive WASM expression coverage is partial (numeric-compatible subset)
- Type system does not enforce interface contracts at analysis time
- Function argument types are not validated against parameters
- Analyzer requires `if` conditions to be boolean type (no implicit truthiness coercion)
- JOIN clause parsing in SQL is declared but not implemented

## Forward Declarations (C99)

In ast.h, use `struct X;` not `typedef struct X X;` for types defined elsewhere:
```c
struct Type;   // defined in types.h
struct Scope;  // defined in scope.h
struct DepSet; // defined in deps.h
```

## Testing

Run all ~198 tests across 14 suites:
```bash
make test
```

Individual test suites:
```bash
make test_lexer          # 9 tests
make test_parser         # 19 tests
make test_analyzer       # 17 tests
make test_compiler       # 23 tests
make test_partial_eval   # 18 tests
make test_vm             # 37 tests
make test_codegen        # 10 tests
make test_api            # 11 tests
make test_cli            # Shell script integration
make test_debug_metadata # 2 tests
make test_sourcemap      # 5 tests
make test_host_contract  # 3 tests
make test_transpiler     # 2 tests
make test_integration    # 45 end-to-end tests
```

### Pre-existing Known Test Issues
- `test_host_contract`: `component_lookup_path_namespace` fails due to VM execution error in linked component resolution path
- `test_vm`: `ternary_expression` uses `strstr` match due to reactive binding wrapping

## Additional Documentation

- `DESIGN.md` - Complete language design and architecture specification
- `CODEX.md` - Fast-start operational reference
- `docs/LLM_REFERENCE.md` - Comprehensive reference optimized for LLM consumption
- `docs/index.html` - GitHub Pages marketing site
