# Motus

**A compiled XML-based templating language for edge-streamed HTML.**

![Version](https://img.shields.io/badge/version-0.1.0-blue)
![Language](https://img.shields.io/badge/language-C99-orange)
![Bytecode](https://img.shields.io/badge/bytecode-v1.3-green)
![Tests](https://img.shields.io/badge/tests-214-brightgreen)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

> **Heads up:** This project is heavily experimental and was entirely vibecoded. That said, it's built on ideas I've been thinking about for over 5 years. It's mostly a way to give shape to how a language designed specifically for the web could approach problems -- data fetching, rendering, reactivity, edge streaming -- from first principles, without inheriting the assumptions of general-purpose languages.

Motus compiles XML-based templates into compact bytecode that can be streamed and interpreted at the edge. It features position-based syntax, SQL as a first-class citizen, fine-grained dependency tracking, partial evaluation, and scoped component architecture -- all from a single C99 library.

---

## Features

- **Compiled, not interpreted at source level** -- templates are compiled to a compact bytecode format, not parsed at render time
- **Position-based syntax** -- `<let name value>` instead of `<y:let name="value">`, reducing boilerplate
- **SQL as a first-class citizen** -- embed SQL queries directly in declarations with automatic parameterization
- **Partial evaluation** -- static content is resolved at compile time; only dynamic data produces bytecode "holes"
- **Fine-grained dependency tracking** -- tracks `product.name` rather than `product` for precise cache invalidation
- **Streamable bytecode** -- designed for WASM edge execution with buffered streaming output
- **Component architecture** -- `defcomp` with typed props, named slots, CSS scoping, and import/export
- **Pipe syntax** -- chain transformations with `<output value | uppercase | trim>`
- **Reactive state** -- `var`/`set` for component-scoped mutable state
- **Pattern matching** -- `match`/`case`/`default` for clean conditional rendering
- **Scoped CSS and JS extraction** -- `<style>` blocks are extracted and scoped per component
- **Source maps** -- debug metadata maps bytecode offsets back to source locations
- **Full compilation pipeline** -- Source -> Lexer -> Parser -> AST -> Analyzer -> Compiler -> Bytecode -> VM -> HTML

---

## Quick Start

### Prerequisites

- A C99-compatible compiler (GCC, Clang)
- GNU Make
- Node.js/npm (for the edge worker example only)

### Build

```bash
make          # Build libmot.a
make test     # Run all ~198 tests across 14 suites
make cli      # Build the mot command-line tool
make clean    # Clean build artifacts for a full rebuild
```

The build produces:

| Artifact | Description |
|---|---|
| `build/libmot.a` | Static library for embedding in C/C++ projects |
| `build/mot` | CLI tool for compiling `.mot` files (requires `make cli`) |

### Run the Example

The example demonstrates end-to-end compilation on an origin server and bytecode interpretation on a Cloudflare Worker at the edge.

```bash
cd example
./run.sh              # Start both origin (port 8080) and edge (port 8791)
./run.sh origin       # Start only the origin server (no Node.js required)
./run.sh test         # Quick test of origin compilation
```

Open `http://localhost:8791` to see edge-rendered pages compiled from `.mot` source.

---

## Language Guide

### Scoped Bindings

`let` creates a scoped binding visible only within its body:

```xml
<let greeting "Hello, World!">
    <p><output greeting></p>
</let>
```

Bindings can hold computed expressions:

```xml
<let fullName firstName + " " + lastName>
    <h1><output fullName></h1>
</let>
```

### Reactive State

`var` declares mutable component-scoped state. `set` updates it:

```xml
<var counter 0 />
<p>Count: <output counter></p>
<set counter counter + 1 />
```

### Expression Output and Pipes

`output` renders an expression into the HTML stream. Pipes chain transformations:

```xml
<output user.name>
<output user.name | uppercase>
<output price | formatCurrency | trim>
```

### Conditionals

```xml
<if user.isAdmin>
    <p>Welcome, administrator.</p>
<elsif user.isLoggedIn>
    <p>Welcome back, <output user.name>.</p>
<else>
    <p>Please log in.</p>
</if>
```

### Iteration

```xml
<for product in products>
    <div class="card">
        <h3><output product.name></h3>
        <p><output product.price></p>
    </div>
</for>
```

### Pattern Matching

```xml
<match status>
    <case "active">
        <span class="badge green">Active</span>
    </case>
    <case "pending">
        <span class="badge yellow">Pending</span>
    </case>
    <default>
        <span class="badge grey">Unknown</span>
    </default>
</match>
```

### SQL Queries

SQL is embedded directly in `let` bindings. Use `:param` for parameterized queries:

```xml
<!-- Static query (resolved at compile time) -->
<let products select name, price from products where active eq 1>
    <for product in products>
        <output product.name>
    </for>
</let>

<!-- Dynamic query (resolved at runtime via the edge) -->
<let details dynamic select * from products where id = :productId>
    <h1><output details.name></h1>
</let>
```

### Components

Define components with `defcomp`. Props use a pipe-delimited signature with optional types and defaults:

```xml
<defcomp ProductCard | name: string, price: number = 0, category: string = "General" |>
    <div class="product-card">
        <h3><output name></h3>
        <p class="price"><output price></p>
        <span class="category"><output category></span>
        <children />
    </div>
</defcomp>
```

Use the component by name:

```xml
<ProductCard name="Widget" price=9.99 category="Tools">
    <p>Additional content goes here.</p>
</ProductCard>
```

### Named Slots

Components can define named slots for structured content injection:

```xml
<defcomp Layout | title: string |>
    <html>
        <head><title><output title></title></head>
        <body>
            <children />
        </body>
    </html>
</defcomp>
```

### Imports and Exports

```xml
<!-- Import a component -->
<import ProductCard from "components/ProductCard" />
<import Layout from "components/Layout" dynamic>

<!-- Export from a file -->
<export default ProductCard>
```

### Scoped CSS

`<style>` blocks are extracted and scoped to the component at compile time:

```xml
<defcomp Card | title: string |>
    <style>
        .card { border: 1px solid #ddd; border-radius: 8px; padding: 16px; }
        .card h3 { margin: 0 0 8px 0; }
    </style>
    <div class="card">
        <h3><output title></h3>
        <children />
    </div>
</defcomp>
```

### Macros

```xml
<macro icon>
    <svg class="icon"><use href="#icon-name" /></svg>
</macro>
```

### Embedded HTML

Motus is a superset of HTML. Standard HTML elements pass through unchanged:

```xml
<div class="container">
    <h1>This is plain HTML</h1>
    <p>Mixed with <output dynamicContent>.</p>
</div>
```

---

## Architecture

```
Source (.mot)
    |
    v
+----------+     +---------+     +----------+     +----------+
|  Lexer   | --> | Parser  | --> | Analyzer | --> | Compiler |
| (5 modes)|     | (AST)   |     | (scopes, |     | (bytecode|
|          |     |         |     |  types,  |     |  + partial|
|          |     |         |     |  deps)   |     |  eval)   |
+----------+     +---------+     +----------+     +----------+
                                                       |
                                      +----------------+----------------+
                                      |                |                |
                                      v                v                v
                                 +--------+       +--------+      +---------+
                                 |Bytecode|       |  CSS   |      |   JS    |
                                 | (.ybc) |       |(scoped)|      |(extract)|
                                 +--------+       +--------+      +---------+
                                      |
                          +-----------+-----------+
                          |                       |
                          v                       v
                    +----------+            +-----------+
                    |  C VM    |            | WASM/Edge |
                    | (origin) |            | (worker)  |
                    +----------+            +-----------+
                          |                       |
                          v                       v
                    Streaming HTML          Streaming HTML
```

### Source Layout

```
src/
  mot.h                    Public API header
  lexer/                   Multi-mode lexer (XML, EXPR, SQL, STYLE, SCRIPT)
  parser/                  Recursive descent parser, 30+ AST node types
  analyzer/                Scope resolution, type inference, dependency tracking
  compiler/                Bytecode emission with partial evaluation
    bytecode.h             Bytecode format, opcodes, serialization
    compiler.h             Compilation pipeline
    partial_eval.h         Constant folding and static reduction
  runtime/                 Stack-based bytecode interpreter
    vm.h                   VM state, value types, execution API
  codegen/                 CSS/JS extraction with component scoping
  debug/                   Source map generation (bytecode PC -> source location)
  linker/                  Cross-module component reference resolution
  transpiler/              Bytecode-to-WASM compilation
  cli/                     CLI tool entry points
  util/                    Arena allocator, dynamic arrays, string utils, hashing

runtime-wasm/              WASM bytecode interpreter for edge execution
  src/                     C sources compiled to WASM via Emscripten
  host.js                  JavaScript host adapter for WASM runtime
  build.sh                 WASM build script

example/
  origin/                  HTTP server that compiles .mot on demand
  edge/                    Cloudflare Worker that interprets bytecode at the edge
  content/pages/           Example .mot pages (index, products, about, etc.)
  content/components/      Reusable .mot components
  run.sh                   Start both servers for local development

tests/                     ~198 tests across 14 suites
```

### Bytecode Format

All multi-byte values are **little-endian**. The binary format is structured as sequential sections:

| Section | Contents |
|---|---|
| Header | Magic (`0x00544F4D` = "MOT\0"), version, flags |
| Constants | Pool of typed constants (null, bool, int, number, string) |
| Strings | Interned string table |
| Data Requests | SQL query descriptors with parameter bindings |
| Dependencies | Fine-grained dependency paths (e.g., `product.name`) |
| Builtins | Builtin function references |
| Component Refs | Dynamic component references for edge loading |
| Main Chunk | Primary bytecode instructions |
| Function Chunks | One chunk per component/macro definition |
| Debug Trailer | Optional source map spans (magic `0x47424459` = "YDBG") |

The instruction set uses `BC_*` prefixed opcodes (60+ instructions) covering stack operations, arithmetic, comparison, control flow, iteration, streaming HTML output, data fetching, function calls, component lifecycle, and dependency regions.

---

## API Reference

### Core Compilation

```c
#include "mot.h"

/* Compile source to bytecode + extracted CSS + JS */
MotCompileResult mot_compile(const char *source, size_t source_len);

/* Compile with explicit options */
MotCompileResult mot_compile_with_options(
    const char *source,
    size_t source_len,
    const MotCompileOptions *options
);

/* Free all memory associated with a compilation result */
void mot_result_free(MotCompileResult *result);
```

### Compilation Options

```c
typedef struct {
    MotCompileTarget target;    /* MOT_TARGET_BYTECODE, MOT_TARGET_WASM, MOT_TARGET_BOTH */
    bool partial_eval;          /* Enable partial evaluation (static folding) */
    bool include_debug;         /* Include debug/source map metadata in bytecode */
    MotLinkedComponentResolverFn linked_component_resolver;
    void *linked_component_userdata;
} MotCompileOptions;
```

### Compilation Result

```c
typedef struct {
    uint8_t *bytecode;          /* Compiled bytecode (caller must free via mot_result_free) */
    size_t bytecode_len;
    uint8_t *wasm;              /* WASM output (if target includes WASM) */
    size_t wasm_len;
    char *source_map_json;      /* JSON source map (if debug enabled) */
    char *css;                  /* Extracted and scoped CSS */
    char *js;                   /* Extracted JavaScript */
    MotErrorList errors;        /* Compilation errors */
} MotCompileResult;
```

### Parse-Only API (for tooling)

```c
/* Parse source into an AST without compiling */
struct AstNode *mot_parse(
    const char *source,
    size_t source_len,
    struct Arena *arena,
    MotErrorList *errors
);
```

### VM Execution

```c
#include "runtime/vm.h"

/* Create and initialize a VM */
VM *vm = vm_new(arena);
vm_init(vm, module);

/* Set streaming output callback */
vm_set_output(vm, output_callback, userdata);

/* Set data fetch callback for SQL queries */
vm_set_fetch(vm, fetch_callback, userdata);

/* Set dynamic component loader */
vm_set_component_loader(vm, component_callback, userdata);

/* Execute bytecode */
VMResult result = vm_run(vm);  /* VM_OK, VM_ERROR, or VM_AWAIT_DATA */
```

### Error Handling

```c
typedef struct {
    const char *message;
    const char *file;
    int line;
    int column;
} MotError;

typedef struct {
    MotError *errors;
    size_t count;
    size_t capacity;
} MotErrorList;
```

### Usage Example

```c
#include "mot.h"
#include <stdio.h>

int main(void) {
    const char *source = "<let msg \"Hello\"><p><output msg></p></let>";

    MotCompileResult result = mot_compile(source, strlen(source));

    if (result.errors.count > 0) {
        for (size_t i = 0; i < result.errors.count; i++) {
            fprintf(stderr, "Error at line %d: %s\n",
                    result.errors.errors[i].line,
                    result.errors.errors[i].message);
        }
    } else {
        printf("Bytecode: %zu bytes\n", result.bytecode_len);
        if (result.css) printf("CSS: %s\n", result.css);
    }

    mot_result_free(&result);
    return 0;
}
```

Compile and link against the library:

```bash
gcc -std=c99 -Isrc my_program.c -Lbuild -lmot -o my_program
```

---

## Project Status

**Current version: 0.1.0** | **Bytecode version: 1.3**

### Test Coverage

~214 tests across 14 suites:

| Suite | Area |
|---|---|
| Lexer | Multi-mode tokenization (XML, EXPR, SQL, STYLE, SCRIPT) |
| Parser | AST construction for all 30+ node types |
| Analyzer | Scope resolution, type inference, dependency tracking |
| Compiler | Bytecode emission and instruction correctness |
| Partial Eval | Constant folding and static reduction |
| VM / Runtime | Bytecode interpretation and streaming output |
| Codegen | CSS/JS extraction and component scoping |
| API | Public API surface and error handling |
| CLI | Command-line tool integration |
| Debug Metadata | Debug span recording |
| Source Maps | Bytecode-to-source mapping |
| Host Contract | WASM host callback interface |
| Transpiler | Bytecode-to-WASM compilation |
| Integration | End-to-end compilation and execution |

### Known Limitations

- SQL keywords (`count`, `sum`, `avg`, etc.) are reserved in all lexer modes -- use alternative names like `counter` instead of `count` for variable bindings
- WASM target from the core compiler is not yet fully implemented; use the `runtime-wasm/` build pipeline for edge execution
- Reactive WASM expression coverage is partial
- The type system does not yet enforce interface contracts
- Comparison operators (`<`, `>`, `==`) inside `<output>` tags can conflict with XML syntax

---

## Contributing

Contributions are welcome. To get started:

1. Fork the repository and create a feature branch.
2. Ensure all tests pass:
   ```bash
   make clean && make test
   ```
3. Follow the existing C99 style conventions:
   - `snake_case` for functions and variables
   - `PascalCase` for type names
   - `BC_*` prefix for bytecode opcodes, `OP_*` for AST operators, `NODE_*` for AST node types
4. Add tests for new functionality in the appropriate `tests/` subdirectory.
5. Submit a pull request with a clear description of your changes.

### Development Notes

- Activate the project toolchain before building: `eval "$(mise activate bash)"`
- The arena allocator (`src/util/arena.h`) is used throughout; avoid manual `malloc`/`free` in compiler internals
- Forward declarations in `ast.h` use `struct X;` (not `typedef struct X X;`) for types defined in other headers
- `BC_EMIT_TAG_OPEN` emits `<tag` without the closing `>` -- attributes are emitted before `BC_EMIT_TAG_END` writes `>`

---

## License

MIT License. See [LICENSE](LICENSE) for details.
