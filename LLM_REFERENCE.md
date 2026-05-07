# Motus Compiler -- LLM Reference

> This document is optimized for consumption by large language models to help them
> understand, query, debug, and extend the Motus codebase. Every section
> contains precise file paths, struct field names, enum values, and function
> signatures drawn directly from the source.

---

## 1. Project Identity

### What It Is

Motus is a **C99 library** implementing a compiler for an XML-based
templating language called "Motus". It compiles `.mot` source files into:

1. **Streamable bytecode** -- a compact binary format executed by a stack-based VM.
2. **Extracted CSS** -- component-scoped stylesheets.
3. **Extracted JS** -- component-scoped scripts.

The runtime interprets bytecode and streams HTML output, designed for edge
execution (Cloudflare Workers via WASM) with asynchronous data fetching
(SQL queries resolved at runtime).

### Who It Is For

- Web developers authoring server-rendered pages with embedded SQL data queries.
- Edge-compute platforms that stream HTML from precompiled bytecode.
- Tooling authors building IDE support, linters, or formatters for `.mot` files.

### Full Compilation Pipeline

```
.mot source
    |
    v
  Lexer  (src/lexer/)        -- multi-mode tokenizer (XML/EXPR/SQL)
    |
    v
  Parser (src/parser/)       -- recursive descent, produces AST
    |
    v
  AST    (src/parser/ast.h)  -- tagged union tree with linked-list siblings
    |
    v
  Analyzer (src/analyzer/)   -- scope resolution, type checking, dependency tracking
    |
    v
  Compiler (src/compiler/)   -- bytecode emission with optional partial evaluation
    |
    v
  BytecodeModule             -- in-memory bytecode + metadata
    |
    +---> bytecode_serialize() --> binary bytecode blob
    +---> codegen_extract()    --> extracted CSS and JS strings
    |
    v
  VM (src/runtime/)          -- stack-based interpreter, streams HTML via callbacks
```

### Repository Structure

```
motus/
  Makefile                   # Build system (gcc, ar)
  mise.toml                  # Tool versions (node 20 for examples)
  CLAUDE.md                  # Claude Code project instructions
  DESIGN.md                  # Language design document
  src/                       # Core compiler library (C99)
    mot.h                    # Public API header
    mot.c                    # Public API implementation
    lexer/
      tokens.h               # TokenType enum, Token struct
      lexer.h                # Lexer struct, lexer_init/next/peek
      lexer.c                # Multi-mode lexer implementation
    parser/
      ast.h                  # NodeType, OpType, LiteralType enums; AstNode struct
      ast.c                  # AST node constructors and utilities
      parser.h               # Parser struct, parser_init/parse
      parser.c               # Recursive descent parser
    analyzer/
      types.h                # TypeKind enum, Type/TypeField/TypeContext structs
      types.c                # Type system implementation
      scope.h                # ScopeKind, SymbolKind enums; Scope/Symbol structs
      scope.c                # Scope/symbol table implementation
      deps.h                 # DepPath, DepSet, DepContext structs
      deps.c                 # Dependency tracking implementation
      analyzer.h             # Analyzer struct, analyzer_new/analyze
      analyzer.c             # Semantic analysis implementation
    compiler/
      bytecode.h             # OpCode enum, BytecodeModule/Chunk/Constant structs
      bytecode.c             # Bytecode module, serialization/deserialization
      compiler.h             # Compiler struct, compiler_new/compile
      compiler.c             # Bytecode code generation
      partial_eval.h         # PEValueKind enum, PartialEval/PEValue structs
      partial_eval.c         # Compile-time constant folding
    runtime/
      vm.h                   # ValueType enum, VM/Value/CallFrame structs
      vm.c                   # Stack-based bytecode interpreter
    codegen/
      codegen.h              # CSSBlock, JSBlock, CodegenResult structs
      codegen.c              # CSS/JS extraction from AST
      css.c                  # CSS selector scoping
    debug/
      sourcemap.h            # SourceMapBuilder, VLQ encoding
      sourcemap.c            # Source map generation
    linker/
      bytecode_linker.h      # mot_link_resolve_module()
      bytecode_linker.c      # Cross-module component linking
    transpiler/
      bytecode_to_wasm.h     # mot_transpile_component_bytecode_to_wasm()
      bytecode_to_wasm.c     # Bytecode-to-WASM transpiler
    cli/
      mot_cli.c              # Command-line compiler frontend
      mot_bytecode_transpiler_cli.c  # WASM transpiler CLI
    util/
      arena.h / arena.c      # Arena (bump) allocator
      str.h / str.c          # String view utilities (StrView)
      vec.h / vec.c          # Type-generic dynamic array macros
      hash.h / hash.c        # Hash map (string keys, open addressing)
  runtime-wasm/              # Freestanding WASM runtime
    src/
      host.h                 # Host callback interface (WASM imports/exports)
      stream.h / stream.c    # Buffered streaming output with HTML escaping
      interpreter.c          # WASM bytecode interpreter core
      wasm_vm.c              # WASM VM adapter
  example/                   # Edge streaming demonstration
    origin/
      server.c               # C HTTP server compiling .mot on demand
      seed.sql               # SQLite seed data
      Makefile               # Origin server build
    edge/src/
      worker.js              # Cloudflare Worker entry point
      wasm-vm.js             # JavaScript WASM VM wrapper
    content/
      pages/*.mot            # Example page templates
      components/*.mot       # Example component templates
    run.sh                   # Start both origin and edge servers
  tests/                     # Test suites (plain C, custom harness)
    lexer/test_lexer.c
    parser/test_parser.c
    analyzer/test_analyzer.c
    compiler/test_compiler.c
    compiler/test_partial_eval.c
    runtime/test_vm.c
    codegen/test_codegen.c
    api/test_api.c
    debug/test_debug_metadata.c
    debug/test_sourcemap.c
    host/test_contract.c
    host/test_transpiler.c
    integration/test_end_to_end.c
  build/                     # Build output (libmot.a, test binaries, CLI)
```

---

## 2. Build System

### Prerequisites

- **GCC** (or any C99-compatible compiler)
- **Make**
- **mise** (optional, only for examples that use node/npm)

### Build Commands

```bash
make              # Build libmot.a static library (default target)
make lib          # Same as above
make cli          # Build build/mot CLI compiler
make transpiler   # Build build/mot-bytecode-transpiler CLI
make test         # Build and run all test suites
make clean        # Remove build/ directory
```

### Individual Test Targets

```bash
make test_lexer
make test_parser
make test_analyzer
make test_compiler
make test_partial_eval
make test_vm
make test_codegen
make test_api
make test_cli
make test_debug_metadata
make test_sourcemap
make test_host_contract
make test_transpiler
make test_integration
```

### Compiler Flags

```
CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -g -Isrc
```

All source files under `src/` (excluding `src/cli/`) are compiled into
`build/libmot.a`. Test and CLI binaries link against this static library with
`-Lbuild -lmot`.

### Environment Setup for Examples

```bash
eval "$(mise activate bash)"   # or zsh -- provides node 20
cd example
./run.sh                       # Starts origin:8080 and edge:8787
```

### CLI Usage

```bash
build/mot --input file.mot \
          --target bytecode \
          --bytecode-out out.ybc \
          --css-out out.css \
          --js-out out.js \
          --map-out out.map \
          --linked-manifest manifest.txt
```

Options: `--target bytecode|wasm|both`, `--no-partial-eval`, `--no-debug`.

---

## 3. Architecture -- Module by Module

### 3.1 Lexer (`src/lexer/`)

**Files:** `tokens.h`, `lexer.h`, `lexer.c`

The lexer operates in five modes, switched by the parser as context changes:

| Mode | Enum | Purpose |
|------|------|---------|
| XML | `LEX_MODE_XML` | HTML/XML tags, attributes, text content |
| Expression | `LEX_MODE_EXPR` | Expressions inside `<output>`, attribute values, etc. |
| SQL | `LEX_MODE_SQL` | SQL queries inside `<let ... select ...>` |
| Style | `LEX_MODE_STYLE` | Raw content inside `<style>` blocks |
| Script | `LEX_MODE_SCRIPT` | Raw content inside `<script>` blocks |

**Key struct:**
```c
typedef struct {
    const char *source;
    size_t source_len;
    size_t pos;
    int line, column;
    LexerMode mode;
    int angle_bracket_depth;    // for nested <fn <arg>> syntax
    bool in_element_content;    // after '>' before '<', preserve whitespace
    bool had_error;
    char error_msg[256];
} Lexer;
```

**Key functions:**
```c
void lexer_init(Lexer *lex, const char *source, size_t source_len);
Token lexer_next(Lexer *lex);       // consume and return next token
Token lexer_peek(Lexer *lex);       // peek without consuming
void lexer_set_mode(Lexer *lex, LexerMode mode);
```

**Token struct:**
```c
typedef struct {
    TokenType type;
    const char *start;   // pointer into source buffer
    size_t length;
    int line, column;
} Token;
```

**TokenType enum** -- 88 variants covering XML structure (`TOK_LT`, `TOK_GT`,
`TOK_LT_SLASH`, `TOK_SLASH_GT`, `TOK_EQ`), literals (`TOK_IDENT`,
`TOK_STRING`, `TOK_NUMBER`, `TOK_TRUE`, `TOK_FALSE`, `TOK_NULL`), Motus
keywords (`TOK_LET`, `TOK_VAR`, `TOK_SET`, `TOK_IF`, `TOK_ELSIF`, `TOK_ELSE`,
`TOK_FOR`, `TOK_IN`, `TOK_MATCH`, `TOK_CASE`, `TOK_DEFAULT`, `TOK_DEFCOMP`,
`TOK_MACRO`, `TOK_IMPORT`, `TOK_EXPORT`, `TOK_OUTPUT`, `TOK_CHILDREN`,
`TOK_DYNAMIC`, `TOK_SINGLE`, `TOK_INTERFACE`, `TOK_SLOTS`, `TOK_STYLE`,
`TOK_SCRIPT`, `TOK_FILL`, `TOK_EXTERNAL`, `TOK_FROM`, `TOK_AS`, `TOK_THEN`,
`TOK_REQUIRE_AUTH`), SQL keywords (`TOK_SELECT`, `TOK_WHERE`, `TOK_ORDER`,
`TOK_BY`, `TOK_LIMIT`, `TOK_OFFSET`, `TOK_JOIN`, `TOK_LEFT`, `TOK_RIGHT`,
`TOK_INNER`, `TOK_OUTER`, `TOK_ON`, `TOK_ASC`, `TOK_DESC`, `TOK_COUNT`,
`TOK_SUM`, `TOK_AVG`, `TOK_MIN`, `TOK_MAX`, `TOK_BETWEEN`, `TOK_LIKE`,
`TOK_IS`), comparison keywords (`TOK_LT_KW`, `TOK_GT_KW`, `TOK_LTE`,
`TOK_GTE`, `TOK_EQ_KW`, `TOK_NEQ`), boolean operators (`TOK_AND`, `TOK_OR`,
`TOK_NOT`), arithmetic operators (`TOK_PLUS`, `TOK_MINUS`, `TOK_STAR`,
`TOK_SLASH`, `TOK_PERCENT`), punctuation (`TOK_PIPE`, `TOK_DOT`, `TOK_COMMA`,
`TOK_COLON`, `TOK_COLON_EQ`, `TOK_LBRACKET`, `TOK_RBRACKET`, `TOK_LBRACE`,
`TOK_RBRACE`, `TOK_LPAREN`, `TOK_RPAREN`, `TOK_DOTDOT`, `TOK_AT`,
`TOK_HASH`), and special tokens (`TOK_TEXT`, `TOK_COMMENT`, `TOK_DOCTYPE`,
`TOK_CDATA`, `TOK_NEWLINE`, `TOK_EOF`, `TOK_ERROR`).

### 3.2 Parser (`src/parser/`)

**Files:** `ast.h`, `ast.c`, `parser.h`, `parser.c`

Recursive descent parser producing an intrusive linked-list AST. The parser
switches the lexer between modes as it enters/exits XML tags, expressions,
SQL clauses, and embedded code blocks.

**Key struct:**
```c
typedef struct Parser {
    Lexer lexer;
    Arena *arena;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;      // error recovery: skip until sync point
    MotErrorList *errors;
} Parser;
```

**Key functions:**
```c
void parser_init(Parser *p, const char *source, size_t source_len,
                 Arena *arena, MotErrorList *errors);
AstNode *parser_parse(Parser *p);              // parse full document
AstNode *parser_parse_element(Parser *p);      // parse single element (testing)
AstNode *parser_parse_expression(Parser *p);   // parse single expression (testing)
bool parser_had_error(Parser *p);
```

### 3.3 AST (`src/parser/ast.h`)

The AST is a tagged union with an intrusive `next` pointer for sibling
linked lists. Each node also carries optional semantic annotations filled
by the analyzer.

**AstNode struct:**
```c
struct AstNode {
    NodeType type;
    int line, column;
    const char *source_path;     // optional, for debug attribution
    union { ... } data;          // node-specific payload (see below)
    AstNode *next;               // sibling linked list
    struct Type *resolved_type;  // set by analyzer
    struct Scope *scope;         // set by analyzer
    struct DepSet *deps;         // set by analyzer
};
```

**NodeType enum** -- 50 variants:

| Category | Values |
|----------|--------|
| Document structure | `NODE_DOCUMENT`, `NODE_ELEMENT`, `NODE_TEXT`, `NODE_COMMENT`, `NODE_DOCTYPE` |
| Motus constructs | `NODE_LET`, `NODE_VAR`, `NODE_SET`, `NODE_OUTPUT`, `NODE_IF`, `NODE_ELSIF`, `NODE_ELSE`, `NODE_FOR`, `NODE_MATCH`, `NODE_CASE`, `NODE_DEFCOMP`, `NODE_MACRO`, `NODE_CHILDREN`, `NODE_IMPORT`, `NODE_EXPORT`, `NODE_INTERFACE`, `NODE_REQUIRE_AUTH` |
| Component parts | `NODE_PROP_DEF`, `NODE_SLOT_DEF`, `NODE_SLOT_FILL`, `NODE_STYLE`, `NODE_SCRIPT` |
| Expressions | `NODE_LITERAL`, `NODE_IDENT`, `NODE_BINARY`, `NODE_UNARY`, `NODE_CALL`, `NODE_PIPE`, `NODE_MEMBER`, `NODE_INDEX`, `NODE_TERNARY`, `NODE_ARRAY`, `NODE_OBJECT`, `NODE_RANGE` |
| SQL | `NODE_SQL`, `NODE_SQL_SELECT`, `NODE_SQL_FROM`, `NODE_SQL_JOIN`, `NODE_SQL_WHERE`, `NODE_SQL_ORDER`, `NODE_SQL_LIMIT`, `NODE_SQL_OFFSET`, `NODE_SQL_COLUMN`, `NODE_SQL_PARAM` |
| Attributes | `NODE_ATTR` |

**Key union members in `data`:**

| Node Type | Union Member | Fields |
|-----------|-------------|--------|
| `NODE_ELEMENT` | `data.element` | `char *tag`, `AstNode *attrs`, `AstNode *children`, `bool self_closing` |
| `NODE_LET`/`NODE_VAR` | `data.binding` | `char *name`, `AstNode *value`, `AstNode *sql`, `AstNode *body`, `bool dynamic`, `bool single` |
| `NODE_SET` | `data.set` | `AstNode *target`, `AstNode *value` |
| `NODE_OUTPUT` | `data.output` | `AstNode *expr` |
| `NODE_IF`/`NODE_ELSIF` | `data.if_stmt` | `AstNode *condition`, `AstNode *then_body`, `AstNode *else_branch` |
| `NODE_FOR` | `data.for_loop` | `char *item`, `char *index`, `AstNode *iterable`, `AstNode *body` |
| `NODE_MATCH` | `data.match` | `AstNode *value`, `AstNode *cases` |
| `NODE_CASE` | `data.case_stmt` | `AstNode *pattern`, `AstNode *body`, `bool is_default` |
| `NODE_DEFCOMP` | `data.defcomp` | `char *name`, `AstNode *props`, `AstNode *slots`, `AstNode *body` |
| `NODE_MACRO` | `data.macro` | `char *name`, `AstNode *params`, `AstNode *body` |
| `NODE_IMPORT` | `data.import` | `AstNode *names`, `char *from_path`, `bool is_external`, `bool is_dynamic`, `char *as_name` |
| `NODE_LITERAL` | `data.literal` | `LiteralType lit_type`, union `v` with `double number`, `int64_t integer`, `bool boolean`, `{char *value, size_t length} string` |
| `NODE_BINARY` | `data.binary` | `OpType op`, `AstNode *left`, `AstNode *right` |
| `NODE_UNARY` | `data.unary` | `OpType op`, `AstNode *operand` |
| `NODE_CALL` | `data.call` | `char *name`, `AstNode *args` (linked list) |
| `NODE_PIPE` | `data.pipe` | `AstNode *input`, `AstNode *stages` (linked list) |
| `NODE_MEMBER` | `data.member` | `AstNode *object`, `char *member` |
| `NODE_INDEX` | `data.index` | `AstNode *object`, `AstNode *index` |
| `NODE_SQL` | `data.sql` | `AstNode *columns`, `AstNode *from`, `AstNode *joins`, `AstNode *where`, `AstNode *order`, `AstNode *limit`, `AstNode *offset` |

**OpType enum:**
```
OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
OP_LT, OP_GT, OP_LTE, OP_GTE, OP_EQ, OP_NEQ,
OP_AND, OP_OR, OP_NOT, OP_NEG
```

**LiteralType enum:** `LIT_STRING`, `LIT_NUMBER`, `LIT_INT`, `LIT_BOOL`, `LIT_NULL`

### 3.4 Analyzer (`src/analyzer/`)

**Files:** `analyzer.h`, `analyzer.c`, `scope.h`, `scope.c`, `types.h`, `types.c`, `deps.h`, `deps.c`

Performs four tasks on the AST:
1. **Scope resolution** -- builds a lexical scope chain, resolves variable references.
2. **Type checking** -- infers and checks types using a structural type system.
3. **Dependency tracking** -- records fine-grained data paths (e.g., `product.name`).
4. **Static/dynamic classification** -- marks expressions that can be resolved at compile time.

**Analyzer struct:**
```c
typedef struct Analyzer {
    Arena *arena;
    TypeContext *types;
    DepContext *deps;
    Scope *scope;            // current scope
    Scope *global_scope;     // module-level scope
    AnalysisError *errors;   // linked list
    AnalysisError **error_tail;
    int error_count;
    bool strict_types;
    bool track_deps;
} Analyzer;
```

**ExprResult** (returned by `analyzer_analyze_expr`):
```c
typedef struct {
    Type *type;
    DepSet *deps;
    bool is_static;      // evaluable at compile time
    bool is_constant;    // literal constant
} ExprResult;
```

**ScopeKind enum:** `SCOPE_GLOBAL`, `SCOPE_COMPONENT`, `SCOPE_MACRO`, `SCOPE_LET`, `SCOPE_FOR`, `SCOPE_IF`, `SCOPE_MATCH`

**SymbolKind enum:** `SYM_LET`, `SYM_VAR`, `SYM_PARAM`, `SYM_COMPONENT`, `SYM_MACRO`, `SYM_SLOT`, `SYM_IMPORT`, `SYM_BUILTIN`

**Symbol struct:**
```c
typedef struct Symbol {
    char *name;
    SymbolKind kind;
    SymbolFlags flags;        // SYM_FLAG_DYNAMIC, SYM_FLAG_SINGLE, SYM_FLAG_EXPORTED, SYM_FLAG_USED
    struct Type *type;
    struct AstNode *def_node;
    int def_line, def_col;
    struct Symbol *next;      // hash bucket chain
} Symbol;
```

**Scope struct:**
```c
typedef struct Scope {
    ScopeKind kind;
    struct Scope *parent;
    Symbol *symbols[32];      // hash table (SCOPE_BUCKETS = 32)
    Arena *arena;
    int depth;
    Symbol *component;        // enclosing component (if any)
    Symbol *macro;            // enclosing macro (if any)
} Scope;
```

#### Type System

**TypeKind enum:** `TYPE_UNKNOWN`, `TYPE_ANY`, `TYPE_VOID`, `TYPE_NULL`, `TYPE_BOOL`, `TYPE_INT`, `TYPE_NUMBER`, `TYPE_STRING`, `TYPE_ARRAY`, `TYPE_OBJECT`, `TYPE_TUPLE`, `TYPE_UNION`, `TYPE_OPTIONAL`, `TYPE_FUNCTION`, `TYPE_COMPONENT`, `TYPE_SLOT`

**Type struct:**
```c
struct Type {
    TypeKind kind;
    union {
        struct { Type *element; } array;
        struct { TypeField *fields; int field_count; } object;
        struct { Type **members; int member_count; } tunion;
        struct { Type *inner; } optional;
        struct { Type **params; int param_count; Type *ret; bool variadic; } func;
        struct { char *name; TypeField *props; TypeField *slots; } component;
    } data;
    unsigned int hash;
};
```

**TypeContext** caches primitive singletons: `t_unknown`, `t_any`, `t_void`, `t_null`, `t_bool`, `t_int`, `t_number`, `t_string`, `t_string_array`, `t_int_array`, `t_number_array`.

**Built-in functions** registered by `analyzer_register_builtins()`:

| Function | Signature | Category |
|----------|-----------|----------|
| `uppercase` | `(string) -> string` | String |
| `lowercase` | `(string) -> string` | String |
| `trim` | `(string) -> string` | String |
| `length` | `(any) -> int` | String/Array |
| `concat` | `(string, string) -> string` | String |
| `abs` | `(number) -> number` | Numeric |
| `round` | `(number) -> int` | Numeric |
| `floor` | `(number) -> int` | Numeric |
| `ceil` | `(number) -> int` | Numeric |
| `first` | `(any) -> any` | Array |
| `last` | `(any) -> any` | Array |
| `reverse` | `(any) -> any` | Array |
| `sort` | `(any) -> any` | Array |
| `now` | `() -> string` | Date/time |
| `formatDate` | `(string, string) -> string` | Date/time |
| `typeof` | `(any) -> string` | Type checking |
| `defined` | `(any) -> bool` | Type checking |
| `json` | `(any) -> string` | JSON |
| `escape` | `(string) -> string` | HTML |
| `raw` | `(string) -> string` | HTML |

#### Dependency Tracking

Fine-grained paths like `user.profile.name` (not just `user`). Used for
cache invalidation and partial re-rendering.

**Key structs:**
```c
typedef struct DepPath {
    DepSegment *segments;    // linked list: name | is_index + index
    int segment_count;
    char *string_repr;       // cached: "user.profile.name"
    unsigned int hash;
    struct DepPath *next;
} DepPath;

typedef struct DepSet {
    DepPath *paths;          // linked list
    int count;
    Arena *arena;
} DepSet;
```

### 3.5 Compiler (`src/compiler/`)

**Files:** `compiler.h`, `compiler.c`, `bytecode.h`, `bytecode.c`, `partial_eval.h`, `partial_eval.c`

Walks the analyzed AST and emits bytecode instructions into `Chunk` buffers.
Supports optional partial evaluation that resolves static expressions at
compile time.

**Compiler struct (key fields):**
```c
typedef struct Compiler {
    Arena *arena;
    BytecodeModule *module;
    Chunk *current_chunk;        // currently emitting to
    CompilerScope *scope;
    int scope_depth;
    Analyzer *analyzer;
    ComponentEntry *components;  // component registry
    MacroEntry *macros;          // macro registry
    DynamicImportEntry *dynamic_imports;
    uint16_t current_component_slot;
    bool in_component;
    bool in_macro;
    AstNode *macro_children;
    bool had_error;
    const char *error_msg;
    int error_line, error_col;
    bool partial_eval;
    PartialEval *pe;
    CompilerLinkedComponentResolverFn linked_component_resolver;
    void *linked_component_userdata;
} Compiler;
```

**CompilerScope struct:**
```c
typedef struct CompilerScope {
    struct CompilerScope *enclosing;
    Local *locals;               // linked list of locals
    int local_count;
    int scope_depth;
    int loop_start;              // for continue
    int loop_end_jump;           // for break
} CompilerScope;
```

**Local struct:**
```c
typedef struct Local {
    const char *name;
    int depth;
    uint16_t slot;
    bool is_captured;
    struct Local *next;
} Local;
```

**Emission pattern:**
```c
// Helper to emit a single opcode byte
static void emit_byte(Compiler *c, uint8_t byte) {
    chunk_write(c->current_chunk, byte, c->current_line, c->arena);
}

// Emit opcode with u16 operand
static void emit_op_u16(Compiler *c, OpCode op, uint16_t operand) {
    emit_byte(c, op);
    chunk_write_u16(c->current_chunk, operand, c->current_line, c->arena);
}
```

**Jump pattern:**
```c
int jump_offset = emit_jump(c, BC_JUMP_IF_FALSE);  // emit placeholder
// ... body ...
patch_jump(c, jump_offset);                          // patch with actual offset
```

**For-loop iterator slot reservation:**
```c
emit_byte(c, BC_ITER_START);
c->scope->local_count++;    // reserve slot for iterator
// ... loop body ...
c->scope->local_count--;    // free iterator slot
```

**AST op to bytecode mapping:**
```c
switch (node->data.binary.op) {
    case OP_ADD: emit_byte(c, BC_ADD); break;
    case OP_SUB: emit_byte(c, BC_SUB); break;
    // ... etc
}
```

#### Partial Evaluation

Resolves static expressions at compile time, leaving "holes" for runtime data.

**PEValueKind enum:** `PE_UNKNOWN`, `PE_NULL`, `PE_BOOL`, `PE_INT`, `PE_NUMBER`, `PE_STRING`, `PE_ARRAY`, `PE_OBJECT`, `PE_DYNAMIC`

The partial evaluator maintains its own scope chain parallel to the compiler's.
When a value is known static, the compiler can emit `BC_CONST` instead of
computing it at runtime.

### 3.6 Bytecode Module (`src/compiler/bytecode.h`)

**BytecodeModule struct:**
```c
typedef struct {
    uint32_t magic;            // BYTECODE_MAGIC (0x00544F4D, "MOT\0")
    uint16_t version_major;    // 1
    uint16_t version_minor;    // 2
    uint16_t flags;            // BYTECODE_FLAG_AUTH_REQUIRED, BYTECODE_FLAG_ADMIN_REQUIRED
    Constant *constants;       // constants pool
    uint32_t const_count, const_cap;
    char **strings;            // interned string table
    uint32_t string_count, string_cap;
    DataRequirement *data_reqs;  // SQL queries for runtime
    uint32_t data_req_count, data_req_cap;
    Dependency *deps;            // cache invalidation paths
    uint32_t dep_count, dep_cap;
    BuiltinRef *builtins;        // builtin function references
    uint32_t builtin_count, builtin_cap;
    ComponentRef *comp_refs;     // dynamic component references
    uint32_t comp_ref_count, comp_ref_cap;
    Chunk main;                  // main code chunk
    Chunk *functions;            // function chunks (components, macros)
    uint32_t func_count, func_cap;
    FunctionDebugRef *func_debug;
    uint32_t func_debug_cap;
    BytecodeDebugSpan *debug_spans;
    uint32_t debug_span_count;
    Arena *arena;
} BytecodeModule;
```

**Chunk struct:**
```c
typedef struct {
    uint8_t *code;
    uint32_t code_len, code_cap;
    uint32_t *lines;           // line number per instruction
    uint32_t lines_len, lines_cap;
} Chunk;
```

**Constant types:** `CONST_NULL`, `CONST_BOOL`, `CONST_INT`, `CONST_NUMBER`, `CONST_STRING`

**DataRequirement struct:**
```c
typedef struct {
    char *name;              // binding name
    uint32_t query_ref;      // stable query reference for origin RPC
    uint32_t signature;      // query+param FNV-1a hash for validation
    char *query;             // SQL string (compile-time only, not serialized)
    uint32_t query_len;
    uint32_t line, column;
    char **param_names;      // parameter names (without ':')
    uint16_t *param_slots;   // local slot for each parameter
    uint16_t param_count;
    bool is_single;          // single row vs array result
    bool is_dynamic;
} DataRequirement;
```

### 3.7 Runtime / VM (`src/runtime/`)

**Files:** `vm.h`, `vm.c`

Stack-based bytecode interpreter with streaming HTML output via callbacks.

**VM struct (key fields):**
```c
typedef struct {
    Arena *arena;
    BytecodeModule *module;
    Value stack[VM_STACK_MAX];       // VM_STACK_MAX = 256
    Value *stack_top;
    CallFrame frames[VM_FRAMES_MAX]; // VM_FRAMES_MAX = 64
    int frame_count;
    VMCaptureBuffer captures[VM_CAPTURE_MAX]; // VM_CAPTURE_MAX = 16
    int capture_depth;
    VMOutputFn output_fn;            // HTML output callback
    void *output_userdata;
    VMFetchDataFn fetch_fn;          // SQL data fetch callback
    void *fetch_userdata;
    VMComponentLoadFn component_fn;  // dynamic component load callback
    void *component_userdata;
    VMComponentLinkedLoadFn linked_component_fn;
    void *linked_component_userdata;
    uint16_t *linked_component_func_targets;
    VMComponentStartFn component_start_fn;
    VMComponentEndFn component_end_fn;
    VMStepHookFn step_hook_fn;
    bool awaiting;                   // async data fetch in progress
    VMObject *globals;
    bool had_error;
    char error_msg[256];
    uint64_t instructions_executed;
} VM;
```

**ValueType enum:** `VAL_NULL`, `VAL_BOOL`, `VAL_INT`, `VAL_NUMBER`, `VAL_STRING`, `VAL_ARRAY`, `VAL_OBJECT`, `VAL_ITERATOR`

**Value struct:**
```c
typedef struct Value {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        VMString string;      // { char *data, uint32_t length, uint32_t hash }
        VMArray *array;       // { Value *elements, uint32_t count, capacity }
        VMObject *object;     // { ObjectField *fields, uint32_t count, capacity }
        VMIterator *iterator; // { Value *array, uint32_t count, current }
    } as;
} Value;
```

**VMResult enum:** `VM_OK`, `VM_ERROR`, `VM_AWAIT_DATA`

**CallFrame struct:**
```c
typedef struct {
    Chunk *chunk;
    uint8_t *ip;           // instruction pointer
    Value *slots;          // base of locals on stack
    VMFrameKind kind;      // VM_FRAME_MAIN, VM_FRAME_FUNCTION, VM_FRAME_COMPONENT
    uint16_t func_idx;
} CallFrame;
```

**Callback signatures:**
```c
typedef void (*VMOutputFn)(const char *data, uint32_t len, void *userdata);
typedef Value (*VMFetchDataFn)(const DataRequirement *req, const Value *frame_slots, void *userdata);
typedef Value (*VMComponentLoadFn)(const ComponentRef *ref, const Value *args, uint8_t argc, void *userdata);
```

### 3.8 Codegen (`src/codegen/`)

**Files:** `codegen.h`, `codegen.c`, `css.c`

Extracts `<style>` and `<script>` blocks from the AST. CSS is scoped per
component via `css_scope()`.

```c
CodegenResult *codegen_extract(Arena *arena, AstNode *doc);
char *codegen_combine_css(Arena *arena, CodegenResult *result);
char *codegen_combine_js(Arena *arena, CodegenResult *result);
char *css_scope(Arena *arena, const char *css, size_t length, const char *component);
```

### 3.9 Debug / Source Maps (`src/debug/`)

**Files:** `sourcemap.h`, `sourcemap.c`

Generates v3 source maps with VLQ-encoded mappings.

```c
void sourcemap_builder_init(SourceMapBuilder *b);
bool sourcemap_builder_add_mapping(SourceMapBuilder *b,
    int32_t generated_line, int32_t generated_col,
    int32_t source_index, int32_t source_line, int32_t source_col,
    int32_t name_index);
char *sourcemap_render_json(const char *file, const char *const *sources,
    size_t source_count, const char *const *names, size_t name_count,
    const char *mappings);
```

### 3.10 Linker (`src/linker/`)

**Files:** `bytecode_linker.h`, `bytecode_linker.c`

Resolves `BC_COMPONENT_LINKED` references by merging linked component function
chunks into the parent module. Encodes resolution as dependency markers:
`@linked_ref:<ref_idx>:<func_idx>`.

```c
bool mot_link_resolve_module(
    BytecodeModule *module,
    MotLinkedModuleResolverFn resolver,   // returns BytecodeModule* for a component path
    void *resolver_userdata,
    char *error_buf, size_t error_buf_len
);
```

### 3.11 Transpiler (`src/transpiler/`)

**Files:** `bytecode_to_wasm.h`, `bytecode_to_wasm.c`

Transpiles Motus bytecode to WASM binary format.

```c
MotTranspilerStatus mot_transpile_component_bytecode_to_wasm(
    const uint8_t *bytecode, size_t bytecode_len,
    const MotBytecodeToWasmOptions *options,
    uint8_t **out_wasm, size_t *out_wasm_len,
    char *error_buf, size_t error_buf_len
);
```

Status codes: `MOT_TRANSPILER_OK`, `MOT_TRANSPILER_INVALID_INPUT`, `MOT_TRANSPILER_OOM`, `MOT_TRANSPILER_INTERNAL_ERROR`.

### 3.12 WASM Runtime (`runtime-wasm/`)

**Files:** `host.h`, `stream.h`, `stream.c`, `interpreter.c`, `wasm_vm.c`

Freestanding WASM build of the VM with host-imported callbacks for I/O.

**WASM exports (called from JS):**
```c
int mot_init(const uint8_t *bytecode, uint32_t len);
int mot_render(void);
int mot_step(void);
int mot_render_component(uint32_t func_idx, const char *props_json, uint32_t props_len,
                         const char *children_json, uint32_t children_len);
int mot_resume(uint32_t req_id, const char *json_data, uint32_t len);
int mot_resume_step(uint32_t req_id, const char *json_data, uint32_t len);
void mot_invalidate(const char *dep_path, uint32_t len);
int mot_state(void);
void mot_free(void);
```

**WASM imports (provided by JS host):**
```c
void host_output(const char *data, uint32_t len);
void host_output_text(const char *data, uint32_t len);
void host_fetch_data(uint32_t req_id, uint32_t query_ref, uint32_t signature,
                     const char *name, const char *params_json, uint32_t is_single);
void host_load_component(uint32_t req_id, const char *name, const char *path,
                         const char *props_json, const char *children_json);
void host_error(const char *msg, uint32_t len);
void host_log(const char *msg, uint32_t len);
void host_render_complete(void);
void host_dep_start(const char *path, uint32_t len);
void host_dep_end(void);
```

**Render states:** `MOT_STATE_IDLE=0`, `MOT_STATE_RUNNING=1`, `MOT_STATE_AWAITING=2`, `MOT_STATE_DONE=3`, `MOT_STATE_ERROR=4`

### 3.13 Utility Libraries (`src/util/`)

**Arena allocator** (`arena.h`):
```c
Arena *arena_create(size_t chunk_size);
void arena_destroy(Arena *arena);
void *arena_alloc(Arena *arena, size_t size);     // 8-byte aligned
void *arena_calloc(Arena *arena, size_t count, size_t size);
char *arena_strdup(Arena *arena, const char *str);
char *arena_strndup(Arena *arena, const char *str, size_t n);
void arena_reset(Arena *arena);
```

**String view** (`str.h`):
```c
typedef struct { const char *data; size_t len; } StrView;
StrView sv_from_cstr(const char *cstr);
StrView sv_from_parts(const char *data, size_t len);
bool sv_eq(StrView a, StrView b);
bool sv_eq_cstr(StrView sv, const char *cstr);
StrView sv_trim(StrView sv);
// ... and more
```

**Dynamic array** (`vec.h`) -- type-generic macros:
```c
Vec(int) numbers = {0};
vec_push(&numbers, 42);
vec_get(&numbers, 0);
vec_free(&numbers);
```

**Hash map** (`hash.h`) -- string keys, open addressing, FNV-1a:
```c
HashMap *hashmap_create(void);
bool hashmap_set(HashMap *map, const char *key, void *value);
void *hashmap_get(HashMap *map, const char *key);
bool hashmap_has(HashMap *map, const char *key);
void *hashmap_remove(HashMap *map, const char *key);
```

---

## 4. Language Specification (Concise)

### 4.1 Syntax Overview

Motus is a **superset of HTML** using XML-based syntax. Standard HTML
elements pass through unchanged. Motus constructs are plain XML elements
with no namespace prefixes and **position-based** attributes.

### 4.2 Bindings

```xml
<!-- let: scoped to children -->
<let name value>
  ... children can see name ...
</let>

<!-- var: component-scoped reactive state (no new scope) -->
<var counter 0>

<!-- set: update a var -->
<set counter counter + 1>

<!-- dynamic data with SQL -->
<let products dynamic
     select id, name from products where active eq 1 limit 10>
  ...
</let>

<!-- single row result -->
<let user dynamic single select * from users where id eq :userId>
  ...
</let>
```

**Scoping rule:** `let` creates a `SCOPE_LET` -- the binding is only visible to
child elements between `<let ...>` and `</let>`. `var` does NOT create a new
scope; the variable is visible throughout the enclosing component.

### 4.3 Output / Expressions

```xml
<output expr>              <!-- HTML-escaped interpolation -->
<output expr | fn1 | fn2>  <!-- piped through functions -->
```

**Operator precedence** (highest to lowest):
1. Member/index access: `.`, `[]`
2. Unary: `-`, `not`
3. Multiplicative: `*`, `/`, `%`
4. Additive: `+`, `-`
5. Comparison: `lt`, `gt`, `lte`, `gte`, `eq`, `neq`
6. Logical AND: `and`
7. Logical OR: `or`
8. Ternary: `if ... then ... else ...`
9. Pipe: `|`

**Comparison keywords** (XML-safe, no `<`/`>` characters):
- `lt` (less than), `gt` (greater than)
- `lte` (less than or equal), `gte` (greater than or equal)
- `eq` (equal), `neq` (not equal)

**Boolean operators:** `and`, `or`, `not` (short-circuit evaluation for `and`/`or`)

**Ternary expression:** `if condition then expr1 else expr2`

**Range literal:** `1..10` (produces `NODE_RANGE`)

**Array literal:** `[1, 2, 3]` or `["a" "b" "c"]` (commas optional)

**Object literal:** `{ key: value, key2: value2 }`

### 4.4 Control Flow

```xml
<!-- Conditionals -->
<if condition>
  ...
<elsif other_condition>
  ...
<else>
  ...
</if>

<!-- For loops -->
<for item in collection>...</for>
<for item, index in collection>...</for>
<for n in 1..10>...</for>

<!-- Pattern matching -->
<match value>
  <case "literal">...</case>
  <case pattern>...</case>
  <default>...</default>
</match>
```

### 4.5 Components

```xml
<!-- Definition -->
<defcomp Name | prop1 : type, prop2 : type = default |
  slots slotName : interface>
  <style>...</style>
  <script>...</script>
  <!-- template body -->
  <children>                    <!-- renders passed children -->
</defcomp>

<!-- Usage -->
<Name prop1=value prop2=value>
  <@slotName>
    content for named slot
  </@slotName>
  default children content
</Name>
```

**Prop definition syntax:** `name : type = default` or `name := default` (infer type from default).

### 4.6 Macros

```xml
<macro name |param1, param2|>
  ...
  <children>     <!-- expands invocation body -->
</macro>
```

Macros are expanded at compile time, inherit the caller's scope, and do not
have their own state.

### 4.7 Imports / Exports

```xml
<import Name from "path">
<import Name1, Name2 from "path">
<import * as Alias from "path">
<import fn from "@pkg" external>

<export Name>
<export default Name>
```

### 4.8 SQL (First-Class)

SQL is embedded directly in `let`/`var` declarations:

```xml
<let results dynamic
     select col1, col2
     from table
     join other on table.id = other.table_id
     where col1 eq :param
     order by col2 desc
     limit 10
     offset 20>
  ...
</let>
```

Parameters use `:paramName` syntax. They reference local variables and are
resolved at runtime.

### 4.9 Authentication

```xml
<require-auth>           <!-- any authenticated user -->
<require-auth role="admin">  <!-- specific role required -->
```

Sets `BYTECODE_FLAG_AUTH_REQUIRED` / `BYTECODE_FLAG_ADMIN_REQUIRED` in the
bytecode header flags.

### 4.10 Embedded Code

```xml
<style> .class { color: red; } </style>
<script> function handler() { ... } </script>
```

Extracted by the codegen pass. CSS is scoped per component. JS is concatenated.

---

## 5. Bytecode Format

### 5.1 Binary Layout

All multi-byte values are **little-endian**.

```
HEADER:
  u32   magic          = 0x00544F4D  ("MOT\0")
  u16   version_major  = 1
  u16   version_minor  = 2
  u16   flags          (bit 0: AUTH_REQUIRED, bit 1: ADMIN_REQUIRED)

SECTION COUNTS:
  u32   const_count
  u32   string_count
  u32   data_req_count
  u32   dep_count
  u32   builtin_count
  u32   comp_ref_count
  u32   func_count

CONSTANTS POOL: (for each constant)
  u8    type           (0=NULL, 1=BOOL, 2=INT, 3=NUMBER, 4=STRING)
  ...payload:
    NULL:   u8 (0)
    BOOL:   u8 (0 or 1)
    INT:    i64 (8 bytes)
    NUMBER: f64 (8 bytes)
    STRING: u32 length + [length bytes of data]

STRINGS TABLE: (for each string)
  u32   length
  [length bytes of string data]

DATA REQUIREMENTS: (for each SQL query)
  u32   name_len + [name bytes]
  u8    is_single
  u8    is_dynamic
  u32   query_ref      (stable reference for origin RPC)
  u32   signature      (FNV-1a hash for validation)
  u16   param_count
  (for each param):
    u32   param_name_len + [name bytes]
    u16   param_slot

DEPENDENCIES: (for each dependency path)
  u32   path_len + [path bytes]

BUILTINS: (for each builtin reference)
  u32   name_len + [name bytes]
  u8    min_args
  u8    max_args

COMPONENT REFS: (for each dynamic component)
  u32   name_len + [name bytes]
  u32   path_len + [path bytes]

MAIN CHUNK:
  u32   code_len
  [code_len bytes of bytecode instructions]

FUNCTION CHUNKS: (for each function, i.e., component/macro)
  u32   code_len
  [code_len bytes of bytecode instructions]

OPTIONAL DEBUG TRAILER:
  u32   debug_magic = 0x47424459 ("YDBG")
  u32   span_count
  (for each span):
    u8    chunk_kind     (0=main, 1=function)
    u16   chunk_index
    u32   start_pc
    u32   end_pc
    u32   line
    u32   column
    u16   node_type
  u32   query_count
  (for each query):
    u32   query_ref
    u32   signature
    u32   line
    u32   column
    u16   name_len + [name bytes]
  u32   function_debug_count
  (for each function debug entry):
    u16   func_index
    u16   name_len + [name bytes]
    u16   source_path_len + [source_path bytes]
```

**Note:** The SQL query text itself is NOT serialized into the bytecode.
Only `query_ref` and `signature` are serialized. The origin server maps
`query_ref` back to the actual SQL at request time.

### 5.2 Opcode Table

| Value | Opcode | Operands | Stack Effect | Description |
|-------|--------|----------|-------------|-------------|
| 0 | `BC_NOP` | none | 0 | No operation |
| 1 | `BC_CONST` | u16 idx | +1 | Push constant from pool |
| 2 | `BC_POP` | none | -1 | Discard top of stack |
| 3 | `BC_DUP` | none | +1 | Duplicate top of stack |
| 4 | `BC_LOAD` | u16 slot | +1 | Load local variable |
| 5 | `BC_STORE` | u16 slot | -1 | Store to local variable |
| 6 | `BC_LOAD_GLOBAL` | u16 name_idx | +1 | Load global by string name |
| 7 | `BC_LOAD_FIELD` | u16 field_idx | 0 | Pop obj, push obj.field |
| 8 | `BC_LOAD_INDEX` | none | -1 | Pop idx, pop arr, push arr[idx] |
| 9 | `BC_STORE_FIELD` | u16 field_idx | -2 | Pop val, pop obj, set field |
| 10 | `BC_STORE_INDEX` | none | -3 | Pop val, pop arr, pop idx, set |
| 11 | `BC_NULL` | none | +1 | Push null |
| 12 | `BC_TRUE` | none | +1 | Push true |
| 13 | `BC_FALSE` | none | +1 | Push false |
| 14 | `BC_INT` | i16 val | +1 | Push small integer (-32768..32767) |
| 15 | `BC_ADD` | none | -1 | a + b |
| 16 | `BC_SUB` | none | -1 | a - b |
| 17 | `BC_MUL` | none | -1 | a * b |
| 18 | `BC_DIV` | none | -1 | a / b |
| 19 | `BC_MOD` | none | -1 | a % b |
| 20 | `BC_NEG` | none | 0 | -a |
| 21 | `BC_EQ` | none | -1 | a == b |
| 22 | `BC_NEQ` | none | -1 | a != b |
| 23 | `BC_LT` | none | -1 | a < b |
| 24 | `BC_LTE` | none | -1 | a <= b |
| 25 | `BC_GT` | none | -1 | a > b |
| 26 | `BC_GTE` | none | -1 | a >= b |
| 27 | `BC_AND` | none | -1 | a and b |
| 28 | `BC_OR` | none | -1 | a or b |
| 29 | `BC_NOT` | none | 0 | not a |
| 30 | `BC_JUMP` | i16 offset | 0 | Unconditional jump |
| 31 | `BC_JUMP_IF_FALSE` | i16 offset | 0 | Jump if top is falsy (does NOT pop) |
| 32 | `BC_JUMP_IF_TRUE` | i16 offset | 0 | Jump if top is truthy (does NOT pop) |
| 33 | `BC_ITER_START` | none | 0 | Begin iteration over TOS array |
| 34 | `BC_ITER_NEXT` | i16 offset | +1 | Push next item or jump if done |
| 35 | `BC_ITER_END` | none | -1 | End iteration, clean up |
| 36 | `BC_EMIT_LITERAL` | u16 const_idx | 0 | Emit raw HTML from constant |
| 37 | `BC_EMIT_TEXT` | none | -1 | Emit HTML-escaped text from TOS |
| 38 | `BC_EMIT_RAW` | none | -1 | Emit unescaped HTML from TOS |
| 39 | `BC_EMIT_ATTR_START` | u16 name_idx | 0 | Begin attribute: ` name="` |
| 40 | `BC_EMIT_ATTR_END` | none | 0 | End attribute: `"` |
| 41 | `BC_EMIT_TAG_OPEN` | u16 name_idx | 0 | Emit `<tag` (no closing >) |
| 42 | `BC_EMIT_TAG_END` | none | 0 | Emit `>` |
| 43 | `BC_EMIT_TAG_CLOSE` | u16 name_idx | 0 | Emit `</tag>` |
| 44 | `BC_EMIT_TAG_SELF` | none | 0 | Emit `/>` |
| 45 | `BC_FETCH_DATA` | u16 req_idx | +1 | Request SQL data (may suspend) |
| 46 | `BC_FETCH_WAIT` | none | 0 | Wait for pending data |
| 47 | `BC_CALL` | u16 fn_idx, u8 argc | -(argc-1) | Call function |
| 48 | `BC_CALL_BUILTIN` | u16 builtin_idx, u8 argc | -(argc-1) | Call builtin function |
| 49 | `BC_CALL_PIPE` | u16 fn_idx | 0 | Pipe call (implicit 1 arg on stack) |
| 50 | `BC_RETURN` | none | 0 | Return from function |
| 51 | `BC_COMPONENT_START` | u16 comp_idx | -2 | Start component (pop children, props) |
| 52 | `BC_COMPONENT_END` | none | 0 | End component |
| 53 | `BC_COMPONENT_LOAD` | u16 ref_idx, u8 argc | varies | Load dynamic component |
| 54 | `BC_SLOT_START` | u16 slot_idx | 0 | Start slot region |
| 55 | `BC_SLOT_END` | none | 0 | End slot region |
| 56 | `BC_SLOT_DEFAULT` | none | 0 | Use default slot content |
| 57 | `BC_DEP_START` | u16 dep_idx | 0 | Begin dependency tracking region |
| 58 | `BC_DEP_END` | none | 0 | End dependency tracking region |
| 59 | `BC_ARRAY_NEW` | u16 count | -(count-1) | Create array from TOS values |
| 60 | `BC_OBJECT_NEW` | u16 count | -(2*count-1) | Create object from key/value pairs |
| 61 | `BC_OBJECT_SET` | u16 key_idx | -1 | Set field on TOS object |
| 62 | `BC_CONCAT` | u8 count | -(count-1) | Concatenate strings |
| 63 | `BC_HALT` | none | 0 | End of program |
| 64 | `BC_COMPONENT_LINKED` | u16 ref_idx, u8 argc | varies | Load linked component |

### 5.3 Component Call Convention

`BC_COMPONENT_START` sequence:
1. Read u16 function index.
2. Pop children value from stack.
3. Pop props value from stack.
4. Push a new call frame (return address, code bounds).
5. Jump to function chunk start.
6. Inside the function: slot 0 = props, slot 1 = children.

---

## 6. Public API

### 6.1 Core Functions

**File:** `src/mot.h`

```c
// Compile with default options (bytecode target, partial eval on, debug on)
MotCompileResult mot_compile(const char *source, size_t source_len);

// Compile with explicit options
MotCompileResult mot_compile_with_options(
    const char *source, size_t source_len,
    const MotCompileOptions *options
);

// Free all memory in a compile result
void mot_result_free(MotCompileResult *result);

// Parse only (for tooling -- returns arena-allocated AST)
struct AstNode *mot_parse(
    const char *source, size_t source_len,
    struct Arena *arena, MotErrorList *errors
);
```

### 6.2 Types

```c
typedef struct {
    uint8_t *bytecode;       // heap-allocated, caller owns
    size_t bytecode_len;
    uint8_t *wasm;           // heap-allocated (currently always NULL)
    size_t wasm_len;
    char *source_map_json;   // heap-allocated JSON string
    char *css;               // heap-allocated combined CSS
    char *js;                // heap-allocated combined JS
    MotErrorList errors;     // errors encountered during compilation
} MotCompileResult;

typedef struct {
    MotCompileTarget target;                        // MOT_TARGET_BYTECODE (default)
    bool partial_eval;                              // default: true
    bool include_debug;                             // default: true
    MotLinkedComponentResolverFn linked_component_resolver;  // optional
    void *linked_component_userdata;                // optional
} MotCompileOptions;

typedef enum {
    MOT_TARGET_BYTECODE = 0,
    MOT_TARGET_WASM = 1,     // not yet implemented
    MOT_TARGET_BOTH = 2,     // not yet implemented
} MotCompileTarget;

typedef struct {
    const char *message;
    const char *file;
    int line, column;
} MotError;

typedef struct {
    MotError *errors;
    size_t count, capacity;
} MotErrorList;
```

### 6.3 Memory Management Rules

1. **Arena allocation** -- Most internal data structures (AST nodes, types,
   scopes, bytecode chunks, strings) are allocated from an arena. The arena is
   created at the start of `mot_compile_with_options()` and destroyed at the
   end. Callers never need to free arena-allocated data.

2. **Heap allocation** -- The `MotCompileResult` fields (`bytecode`, `wasm`,
   `source_map_json`, `css`, `js`, `errors.errors`, each error `message`) are
   heap-allocated with `malloc`. Call `mot_result_free()` to free them all.

3. **`mot_parse()` is different** -- The caller provides the arena. The returned
   AST lives in that arena and is freed when the caller destroys it.

### 6.4 Error Handling

- Compilation never crashes; errors are collected in `MotCompileResult.errors`.
- Check `result.errors.count > 0` after compilation.
- Each phase (parse, analyze, compile) may contribute errors.
- Parse errors include line/column. Analysis errors include line/col. Compiler
  errors report the first error only.

### 6.5 Typical Usage Pattern

```c
const char *source = "<let x 42><output x>";
MotCompileResult result = mot_compile(source, strlen(source));

if (result.errors.count > 0) {
    for (size_t i = 0; i < result.errors.count; i++) {
        fprintf(stderr, "Error at line %d: %s\n",
                result.errors.errors[i].line,
                result.errors.errors[i].message);
    }
} else {
    // result.bytecode, result.bytecode_len -- serialized bytecode
    // result.css -- extracted CSS
    // result.js  -- extracted JS

    // To run the bytecode:
    Arena *arena = arena_create(64 * 1024);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode,
                                                (uint32_t)result.bytecode_len, arena);
    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    vm_set_output(vm, my_output_callback, my_userdata);
    vm_set_fetch(vm, my_fetch_callback, my_fetch_userdata);
    VMResult r = vm_run(vm);
    arena_destroy(arena);
}

mot_result_free(&result);
```

---

## 7. Code Patterns

### 7.1 How to Add a New Opcode

1. **Define the opcode** in `src/compiler/bytecode.h`:
   ```c
   typedef enum {
       // ...existing opcodes...
       BC_MY_NEW_OP,        /* description: operands */
       BC_HALT,             /* keep HALT near the end */
       BC_COMPONENT_LINKED,
   } OpCode;
   ```

2. **Add the name** to `opcode_name()` in `src/compiler/bytecode.c`:
   ```c
   case BC_MY_NEW_OP: return "MY_NEW_OP";
   ```

3. **Add disassembly** in `chunk_disassemble()` in `src/compiler/bytecode.c`
   (handle operand printing).

4. **Emit it** in the compiler (`src/compiler/compiler.c`):
   ```c
   emit_byte(c, BC_MY_NEW_OP);
   emit_u16(c, operand);  // if it has a u16 operand
   ```

5. **Execute it** in the VM dispatch loop (`src/runtime/vm.c`):
   ```c
   case BC_MY_NEW_OP: {
       uint16_t operand = READ_U16();
       // ... implementation ...
       break;
   }
   ```

6. **Handle in WASM runtime** (`runtime-wasm/src/interpreter.c` and
   `example/edge/src/wasm-vm.js`).

7. **Add tests** in the appropriate test file(s).

### 7.2 How to Add a New AST Node Type

1. **Add to NodeType** in `src/parser/ast.h`:
   ```c
   NODE_MY_CONSTRUCT,    /* <my-construct ...> */
   ```

2. **Add data union member** in the `AstNode.data` union.

3. **Add constructor** in `src/parser/ast.h` (declaration) and
   `src/parser/ast.c` (implementation):
   ```c
   AstNode *ast_my_construct(Arena *arena, ..., int line, int col);
   ```

4. **Update node_type_name()** in `src/parser/ast.c`.

5. **Parse it** in `src/parser/parser.c` -- add recognition in the appropriate
   parsing function (e.g., `parse_element()` for new tag types).

6. **Analyze it** in `src/analyzer/analyzer.c` -- add a case in
   `analyzer_analyze_node()`.

7. **Compile it** in `src/compiler/compiler.c` -- add a case in
   `compile_node()`.

8. **Add tests** for parser, analyzer, compiler, and integration.

### 7.3 How to Add a New Builtin Function

1. **Register in the analyzer** in `analyzer_register_builtins()`
   (`src/analyzer/analyzer.c`):
   ```c
   sym = scope_define(scope, "myFunc", SYM_BUILTIN, NULL, 0, 0);
   symbol_set_type(sym, type_function(a->types,
       (Type*[]){type_string(a->types)}, 1, type_string(a->types), false));
   ```

2. **Register in the compiler** -- builtins are auto-registered during
   compilation when first referenced via `bytecode_add_builtin()`.

3. **Implement in the VM** -- add a case in the builtin dispatch within
   `vm.c`'s `BC_CALL_BUILTIN` handler:
   ```c
   if (strcmp(name, "myFunc") == 0) {
       // implementation using stack values
   }
   ```

4. **Implement in WASM runtime** -- add the same logic in the JS interpreter.

5. **Optionally add to partial evaluator** if the function can be evaluated
   at compile time.

### 7.4 Testing Patterns

All test files use the same custom harness pattern:

```c
static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(name) do { \
    tests_run++; \
    printf("Running " #name "..."); \
    name(); \
    tests_passed++; \
    printf(" PASSED\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\nFAILED: %s (line %d)\n", #cond, __LINE__); \
        return; \
    } \
} while(0)
```

**Typical test function:**
```c
static void test_my_feature(void) {
    const char *source = "<let x 42><output x>";
    MotCompileResult result = mot_compile(source, strlen(source));
    ASSERT(result.errors.count == 0);
    ASSERT(result.bytecode != NULL);
    ASSERT(result.bytecode_len > 0);

    // Optionally run bytecode:
    Arena *arena = arena_create(64 * 1024);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode,
                                                (uint32_t)result.bytecode_len, arena);
    // ... set up VM, run, check output ...
    arena_destroy(arena);
    mot_result_free(&result);
}
```

**Integration test pattern** (compile + run + check HTML output):
```c
static char output_buf[4096];
static size_t output_len;

static void capture_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    memcpy(output_buf + output_len, data, len);
    output_len += len;
}

static void test_end_to_end(void) {
    output_len = 0;
    const char *src = "<h1>Hello</h1>";
    MotCompileResult res = mot_compile(src, strlen(src));
    ASSERT(res.errors.count == 0);

    Arena *arena = arena_create(64 * 1024);
    BytecodeModule *mod = bytecode_deserialize(res.bytecode,
                                                (uint32_t)res.bytecode_len, arena);
    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    vm_set_output(vm, capture_output, NULL);
    VMResult r = vm_run(vm);
    ASSERT(r == VM_OK);

    output_buf[output_len] = '\0';
    ASSERT(strstr(output_buf, "<h1>Hello</h1>") != NULL);

    arena_destroy(arena);
    mot_result_free(&res);
}
```

---

## 8. Known Issues and Limitations

### 8.1 Reserved SQL Keywords

SQL keywords (`count`, `sum`, `avg`, `min`, `max`, `select`, `where`, `from`,
`order`, `by`, `limit`, `offset`, `join`, `left`, `right`, `inner`, `outer`,
`on`, `asc`, `desc`, `between`, `like`, `is`) are reserved in **all lexer
modes**. This means you cannot use these as variable names anywhere:

```xml
<!-- WRONG: "count" is reserved -->
<let count 42>

<!-- RIGHT: use an alternative name -->
<let counter 42>
```

### 8.2 WASM Target Not Yet Implemented

`MOT_TARGET_WASM` and `MOT_TARGET_BOTH` return an error from the core compiler:
```
"WASM target is not implemented in the core compiler yet; use runtime-wasm build pipeline"
```

WASM generation currently lives in the edge-side reactive module generation
path. The `bytecode_to_wasm` transpiler provides a separate API for individual
component transpilation.

### 8.3 Comparison Operators in XML Context

The `<` and `>` operators cannot be used directly inside XML content because
they conflict with XML tag syntax. Use keyword equivalents:

```xml
<!-- WRONG: XML parse error -->
<if count > 0>

<!-- RIGHT: use keyword -->
<if count gt 0>
```

### 8.4 Type System Enforcement

The type system uses gradual typing with `TYPE_ANY` as an escape hatch. Type
checking is not fully strict by default (`strict_types = false`). Some type
mismatches are permitted when one operand is `TYPE_ANY` or `TYPE_UNKNOWN`.

### 8.5 Reactive Binding System

The reactive binding system (compiler-emitted markers like `@bind`, `@set`,
`@bindattr`, `@exprbind`, etc.) is implemented but the full browser-side
reactive graph and debugger are still in progress. Reactive WASM expression
coverage is partial (numeric-compatible subset); unsupported expressions fall
back to JS evaluation.

### 8.6 Error Recovery

The parser has a `panic_mode` flag for error recovery but recovery is limited.
Multiple parse errors may be reported but accuracy degrades after the first
error. The compiler reports only the first error.

### 8.7 Host Contract Test Failure

The `component_lookup_path_namespace` test in `test_host_contract` currently
fails. This is a known issue with component namespace resolution during linked
component loading.

### 8.8 Stack and Frame Limits

- Value stack: `VM_STACK_MAX = 256` -- deeply nested expressions may overflow.
- Call frames: `VM_FRAMES_MAX = 64` -- deeply nested component trees may overflow.
- Capture buffers: `VM_CAPTURE_MAX = 16` -- nested slot captures are limited.
- Jump offsets: i16 range (-32768 to 32767) -- very large functions may exceed.

---

## 9. Test Coverage Summary

### Test Suite Counts (Actual)

| Suite | File | Count | Status |
|-------|------|-------|--------|
| Lexer | `tests/lexer/test_lexer.c` | 9 | All pass |
| Parser | `tests/parser/test_parser.c` | 19 | All pass |
| Analyzer | `tests/analyzer/test_analyzer.c` | 17 | All pass |
| Compiler | `tests/compiler/test_compiler.c` | 23 | All pass |
| Partial Eval | `tests/compiler/test_partial_eval.c` | 18 | All pass |
| VM | `tests/runtime/test_vm.c` | 37 | All pass |
| Codegen | `tests/codegen/test_codegen.c` | 10 | All pass |
| API | `tests/api/test_api.c` | 11 | All pass |
| CLI | `tests/cli/test_cli_target.sh` | shell | All pass |
| Debug Metadata | `tests/debug/test_debug_metadata.c` | 2 | All pass |
| Source Map | `tests/debug/test_sourcemap.c` | 5 | All pass |
| Host Contract | `tests/host/test_contract.c` | 3 | 2/3 pass (1 known failure) |
| Transpiler | `tests/host/test_transpiler.c` | 2 | All pass |
| Integration | `tests/integration/test_end_to_end.c` | 45 | All pass |
| **Total** | | **~201** | **200 pass, 1 known failure** |

### What Is Tested

- **Lexer:** Token types for XML, expressions, SQL keywords, operators, strings,
  numbers, multi-mode switching.
- **Parser:** All node types, nested structures, attributes, expressions, SQL
  embedded queries, error recovery.
- **Analyzer:** Scope resolution, type inference, dependency tracking, let/var
  scoping rules, component prop validation, builtin recognition.
- **Compiler:** Bytecode generation for all constructs, constant folding,
  for-loop iteration patterns, component compilation, macro expansion.
- **Partial Eval:** Arithmetic folding, string operations, boolean logic,
  static/dynamic classification.
- **VM:** All opcodes, stack operations, HTML emission, iteration, function
  calls, builtin dispatch, component rendering, data fetching, error handling.
- **Codegen:** CSS extraction, JS extraction, CSS component scoping, combining.
- **API:** End-to-end compilation, option handling, error reporting,
  serialization round-trip, WASM target error.
- **Integration:** Full pipeline from source to HTML output for complex pages
  with components, loops, conditionals, SQL, imports.
- **Debug/Source Map:** VLQ encoding, mapping generation, debug trailer
  serialization.

### What Is Not Fully Tested

- WASM transpilation output correctness (basic smoke test only).
- Linked component resolution across multiple modules (1 test fails).
- Edge-specific WASM runtime (tested via separate `test_wasm_js_parity.sh`
  shell script, not in the main C test suite).
- Browser-side reactive binding updates.
- Error messages for all possible malformed input scenarios.

### Running Tests

```bash
make test          # Run all test suites
make test_lexer    # Run just the lexer tests
make test_vm       # Run just the VM tests
# etc.
```

Each test binary reports pass/fail to stdout and exits with code 0 (all pass)
or 1 (any failure). The `make test` target runs all suites sequentially and
stops on the first failure.
