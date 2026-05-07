# Motus Language Design

## Overview

Motus is an XML-based language for building web pages with:
- Partial evaluation (static data resolved at compile, dynamic stays as bytecode)
- Fine-grained dependency tracking for cache invalidation
- Component-based architecture where each component is a server entrypoint
- SQL as a first-class citizen for data queries
- HTMX-inspired partial updates

## Current Implementation Snapshot (February 2026)

Implemented in code now:
- Core parse/analyze/compile/runtime pipeline emits and executes bytecode `1.2`.
- Origin precompiles configured pages/components at startup and caches artifacts in memory.
- Edge runtime currently executes in `wasm` mode only and streams HTML responses.
- Compiler emits reactive metadata markers:
  - direct bindings: `@bind`, `@set`, `@plan`
  - attribute bindings: `@bindattr`, `@planattr`
  - expression bindings: `@exprbind`, `@exprattr`, `@exprdep`, `@planexpr`
- Browser runtime can parse dependency markers from bytecode and apply targeted DOM updates for tracked paths.
- Edge now builds a per-page reactive wasm companion module from expression metadata and embeds it in bootstrap payload.
- Browser runtime loads that per-page reactive wasm module and uses wasm exports for numeric expression updates.
- Browser runtime data/component/page/runtime requests are same-origin via edge proxy endpoints:
  - `/_mot/runtime/wasm`
  - `/_mot/page/{name}`
  - `/_mot/component/{name}`
  - `/_mot/data/{page}`

Current limits:
- Core compiler `--target wasm|both` still returns not-implemented; wasm generation is currently in edge-side reactive module generation path.
- Reactive wasm expression coverage is partial (numeric-compatible subset); unsupported expressions fall back to JS evaluator.
- Full browser debugger/source-map workflow and full reactive graph tooling are still in progress.

## Execution Model

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    SERVER - COMPILE TIME                                 │
│                                                                          │
│  .mot source ──► Parser ──► AST ──► Compiler ──┬──► CSS (per component) │
│       +                              │         ├──► JS  (per component) │
│  static data                         ▼         └──► Bytecode (partial)  │
│                              Partial Evaluation                          │
│                              (resolves static,                           │
│                               keeps dynamic as holes)                    │
└─────────────────────────────────────────────────────────────────────────┘
                                        │
                                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         EDGE - RUNTIME (Streaming)                       │
│                                                                          │
│  Bytecode ──► WASM Interpreter ──► HTML Stream                          │
│                      │                                                   │
│                      ▼                                                   │
│              Host Callback ◄─── QueryRef/signature metadata in bytecode │
│                      │                                                   │
│                      ▼                                                   │
│              Data Provider (DB, API, Cache)                             │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Syntax Design

### Design Principles

1. **No namespace prefixes** - Motus tags are plain XML elements
2. **Position-based attributes** - Natural reading order, less punctuation
3. **SQL is first-class** - Embedded directly in declarations
4. **Scoped bindings** - `let` bindings are visible only to children
5. **Clean expression output** - `<output expr>` for interpolation

---

### Variables & Bindings

#### `let` - Scoped Binding

Bindings are only visible to child elements:

```xml
<!-- Single binding -->
<let title "My Page">
  <h1><output title></h1>
</let>

<!-- Multiple bindings -->
<let count 42
     items ["a" "b" "c"]
     product { id: 2, name: "shirt" }>
  <p>Count: <output count></p>
  <p>Product: <output product.name></p>
</let>

<!-- Nested scoping -->
<let x = 1>
  <let x = 2>
    <output x>  <!-- outputs 2 -->
  </let>
  <output x>    <!-- outputs 1 -->
</let>
```

#### `var` - Component-scoped Reactive State

Reactive variables visible throughout the component. Changes trigger re-render of dependent elements:

```xml
<var counter 0>
<var selectedTab "home">

<!-- Mutation (triggers re-render of elements using these vars) -->
<set counter counter + 1>
<set selectedTab "profile">

<!-- Dynamic reactive data -->
<var cart dynamic single
     select * from carts where id = :cartId>

<!-- When cart changes, only elements depending on cart update -->
<span class="badge"><output cart.count></span>
```

| Binding | Scope | Resolved | Reactive |
|---------|-------|----------|----------|
| `let` | Children only | Compile/first-pass | No |
| `var` | Component | Edge runtime | Yes |

#### Dynamic Variables

Variables that remain unresolved until edge execution:

```xml
<let user dynamic>
  <h1>Hello, <output user.name></h1>
</let>

<!-- With SQL query -->
<let products dynamic
     select id, name, price
     from products
     where active = true
     order by price asc
     limit 10>
  <for item in products>
    <product-card product=item/>
  </for>
</let>
```

---

### Output & Expressions

#### `output` - Expression Interpolation

```xml
<!-- Simple value -->
<span><output user.name></span>

<!-- Arithmetic -->
<span><output count * 2></span>
<span><output price + tax></span>

<!-- Comparison (use keywords for < > to stay XML-valid) -->
<output items.length gt 0>    <!-- greater than -->
<output count lt 100>          <!-- less than -->
<output count gte 10>          <!-- greater than or equal -->
<output count lte 50>          <!-- less than or equal -->
<output a eq b>                <!-- equal -->
<output a neq b>               <!-- not equal -->

<!-- Boolean -->
<output isActive and isVerified>
<output isAdmin or isModerator>
<output not isDisabled>

<!-- Ternary -->
<output if isActive then "Yes" else "No">

<!-- Property access -->
<output product.price.amount>
<output items[0].name>
<output items[index]>
```

#### Function Calls

Functions can be called inline:

```xml
<!-- Single function -->
<output uppercase "hello">           <!-- "HELLO" -->
<output formatCurrency price "USD">  <!-- "$19.99" -->
<output substr name 0 10>            <!-- first 10 chars -->

<!-- Piping with | -->
<output "hello world" |
        uppercase |
        trim |
        default "fallback">

<!-- Piped with arguments -->
<output price |
        formatCurrency "USD" |
        default "$0.00">

<!-- Nested calls -->
<output formatDate <now> "YYYY-MM-DD">

<!-- breakets are optional for function calls but good to desanbiguate -->
<output <formatDate <now> "YYYY-MM-DD">>
```

---

### Control Flow

#### `if` / `elsif` / `else`

```xml
<if user.isAdmin>
  <admin-panel/>
</if>

<if items.length gt 0>
  <item-list items=items/>
<elsif loading>
  <spinner/>
<else>
  <empty-state/>
</if>
```

#### `match` - Pattern Matching

```xml
<match status>
  <case "loading">
    <spinner/>
  </case>
  <case "error">
    <error-message/>
  </case>
  <case "success">
    <content/>
  </case>
  <default>
    <unknown-state/>
  </default>
</match>

<!-- With destructuring -->
<match result>
  <case { ok: data }>
    <output data>
  </case>
  <case { error: msg }>
    <error-message message=msg/>
  </case>
</match>
```

---

### Loops

#### `for` - Iteration

```xml
<!-- Array iteration -->
<for item in items>
  <li><output item></li>
</for>

<!-- With index -->
<for item, index in items>
  <li><output index>: <output item></li>
</for>

<!-- Object entries -->
<for key, value in object>
  <dt><output key></dt>
  <dd><output value></dd>
</for>

<!-- Range -->
<for n in 1..10>
  <span><output n></span>
</for>

<!-- With SQL inline -->
<for product in select * from products where category = "shoes" limit 5>
  <product-card product=product/>
</for>
```

---

### Data Queries (SQL First-Class)

SQL is embedded directly in variable declarations:

```xml
<!-- Basic query -->
<let products dynamic
     select id, name, price
     from products
     where active = true
     order by created_at desc
     limit 20>
  ...
</let>

<!-- With parameters (from component props or state) -->
<let categoryProducts dynamic
     select *
     from products
     where category_id = :categoryId
     and price between :minPrice and :maxPrice
     order by :sortField :sortDir
     limit :pageSize
     offset :page * :pageSize>
  ...
</let>

<!-- Joins -->
<let orderDetails dynamic
     select o.id, o.total, u.name as customer_name
     from orders o
     join users u on o.user_id = u.id
     where o.id = :orderId>
  ...
</let>

<!-- Aggregations -->
<let stats dynamic
     select count(*) as total,
            sum(price) as revenue,
            avg(price) as avg_price
     from orders
     where created_at > :startDate>
  ...
</let>

<!-- Single row -->
<let user dynamic single
     select * from users where id = :userId>
  <h1>Hello, <output user.name></h1>
</let>
```

---

### Components

#### Definition
<interface interface1 />
<interface interface2 |label: string| />

```xml
<defcomp MyButton | label : string,  variant : string = "primary", disabled := false | 
  slots icon-before : interface1
        icon-after  : interface2>
  <!-- If there is no default value it is required -->
  <!-- We can infer the type by the value when using := -->

  <!-- Slots -->
  

  <!-- Styles (extracted to CSS) -->
  <style>
    .btn { 
      padding: 0.5rem 1rem; border-radius: 4px; 

    }
    .btn-primary { background: blue; color: white; }
    .btn-secondary { background: gray; color: white; }
    
  </style>

  <!-- Script (extracted to JS) -->
  <script>
    function handleClick(el) {
      el.classList.add('clicked');
    }
  </script>

  <!-- Template -->
  <button class="btn btn-{variant}" disabled=disabled onclick="handleClick(this)">
    <icon-before>
    <output label>
    <icon-after2 label>
  </button>
</defcomp>
```

#### Usage

```xml
<Button label="Click me" variant="secondary" disabled=false>
  <@icon-before>
    <fill slot="icon">
      <svg>...</svg>
    </fill>
  </@icon-before>

  <@icon-after |label|>
    <fill slot="icon">
      <svg>...</svg>
    </fill>
    <output label>
  </@icon-after>
  
</Button>

<!-- With dynamic props -->
<Button label=buttonText disabled=isLoading/>
```

---

### Macros

Compile-time expandable templates. Inherit caller's scope, can have parameters, can expand children.

```xml
<!-- Simple macro (inherits scope) -->
<macro cart-count>
  <span class="badge"><output cart.items.length></span>
</macro>

<!-- Macro with parameters -->
<macro product-list |products|>
  <for product in products>
    <product-card product=product/>
  </for>
</macro>

<!-- Macro that wraps children -->
<macro card |title|>
  <div class="card">
    <h2><output title></h2>
    <children>  <!-- expands whatever was passed inside -->
  </div>
</macro>

<!-- Usage -->
<let cart { items: [1, 2, 3] }>
  <cart-count>  <!-- uses cart from outer scope -->

  <product-list products>  <!-- passes products param -->

  <card title="My Card">
    <p>This content goes where children is</p>
  </card>
</let>
```

**Macro vs Component:**
| | Macro | Component |
|--|-------|-----------|
| Expansion | Compile-time | Runtime |
| Scope | Inherits caller's | Isolated |
| Props | Optional params | Explicit typed props |
| Slots | `<children>` only | Named typed slots |
| State | No own state | Can have `var` |


### Imports & Exports

```xml
<!-- Import components -->
<import Button from "./components/Button.mot">
<import Header, Footer from "./layout.mot">
<import * as Icons from "./icons.mot">

<!-- Import external functions (C ABI / WASM) -->
<import formatDate from "@intl" external>
<import validateEmail from "./validators.wasm" external>
<import md5, sha256 from "@crypto" external>

<!-- Export component for use by other languages -->
<export renderProductCard>
<export default ProductPage>
```

---

### HTML Pass-through

All standard HTML is valid and passes through unchanged:

```xml
<div class="container">
  <h1>Regular HTML</h1>
  <p>This is just normal HTML content.</p>
  <img src="/logo.png" alt="Logo">

  <!-- Motus constructs mixed in -->
  <let greeting = "Hello">
    <p><output greeting>, world!</p>
  </let>
</div>
```

---

## Type System

### Primitive Types
- `string`
- `number` (f64)
- `int` (i64)
- `bool`
- `null`

### Compound Types
- `array<T>` or `T[]`
- `object` / `record<K, V>`
- `optional<T>` or `T?`

### Special Types
- `html` - Safe HTML content
- `sql<T>` - SQL query that resolves to T

---

## Operators Reference

Since `<` and `>` conflict with XML, we use keywords:

| Operation | Keyword | Example |
|-----------|---------|---------|
| Less than | `lt` | `<output x lt 10>` |
| Greater than | `gt` | `<output x gt 10>` |
| Less or equal | `lte` | `<output x lte 10>` |
| Greater or equal | `gte` | `<output x gte 10>` |
| Equal | `eq` | `<output x eq y>` |
| Not equal | `neq` | `<output x neq y>` |
| And | `and` | `<output a and b>` |
| Or | `or` | `<output a or b>` |
| Not | `not` | `<output not x>` |

Arithmetic operators work normally: `+ - * / %`

---

## Dependency Tracking

Every expression is tracked at the field level:

```xml
<let product select * from products where id = :id limit 1>
  <!-- Depends on: product.name -->
  <h1><output product.name></h1>

  <!-- Depends on: product.price.amount, product.price.currency -->
  <span><output product.price.amount | 
                formatCurrency product.price.currency></span>
</let>
```

Bytecode includes:
```
Dependencies:
  - product.name
  - product.price.amount
  - product.price.currency
```

Cache invalidation: Only `product.name` changes → only fragments using `product.name` are invalidated.

---

## Complete Example

```xml
<!DOCTYPE html>
<html>
<head>
  <import Header from "./layout/Header.mot">
  <import ProductCard from "./components/ProductCard.mot">
  <import Pagination from "./components/Pagination.mot">

  <let site { name: "MyShop" }>
    <title><output site.name> - Products</title>
  </let>
</head>
<body>
  <defcomp ProductPage | categorySlug : string, page : int = 0, sort : string = "name asc" |>
    <!-- Reactive data: changes trigger re-render -->
    <var category dynamic single
         select * from categories where slug = :categorySlug>

    <var products dynamic
         select id, name, price, image_url
         from products
         where category_id = :category.id
         order by :sort
         limit 20
         offset :page * 20>

    <var totalCount dynamic single
         select count(*) as count
         from products
         where category_id = :category.id>

    <Header/>

    <main class="container">
      <h1><output category.name></h1>

      <div class="grid">
        <for product in products>
          <ProductCard product=product/>
        </for>
      </div>

      <Pagination
        page=page
        total=totalCount.count
        pageSize=20/>
    </main>
  </defcomp>

  <!-- Instantiate with URL params -->
  <ProductPage
    categorySlug=params.category
    page=params.page
    sort=params.sort/>
</body>
</html>
```

---

## Resolved Decisions

- **Comments**: XML `<!-- -->` only for compatibility
- **`=` in let**: Optional, both `<let x 1>` and `<let x = 1>` valid
- **Reactivity**: `var` marks reactive state, changes trigger re-render of dependents
- **Single values**: `single` keyword for queries returning one row (not array)

## Open Questions

1. **Event handling syntax**: `onclick=<set counter counter + 1>` or different approach?
2. **Client-server boundary**: How does the browser communicate state changes back to edge?
3. **Optimistic updates**: Support for instant UI feedback while server confirms?

---

## Dual Compilation Targets: WASM and VM

### Core Principle

**WASM and bytecode/VM are both first-class compilation targets.** They are not mutually exclusive—a single page render can mix both:

- **WASM modules**: Pre-compiled, deployed ahead of time (layouts, system components, app-defined templates)
- **Bytecode + VM**: Compiled on-demand for user-defined/dynamic content

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              COMPILE TIME                                    │
│                                                                              │
│  .mot source ──► Compiler ──┬──► Bytecode (.motb)   [dynamic content]       │
│                             ├──► WASM module (.wasm) [static components]    │
│                             ├──► Source maps (.map)                         │
│                             └──► CSS/JS assets                              │
│                                                                              │
│  Both targets supported throughout the entire stack.                        │
│  Choice depends on deployment model, not runtime environment.               │
└─────────────────────────────────────────────────────────────────────────────┘
```

### When to Use Each Target

| Scenario | Target | Reason |
|----------|--------|--------|
| App-defined layouts/headers | WASM | Pre-compiled, deployed with app |
| System components (Button, Modal) | WASM | Known at build time |
| User-defined pages (CMS) | Bytecode + VM | Compiled on-demand |
| User-uploaded components | Bytecode + VM | Can't pre-deploy |
| Mixed page (app shell + user content) | Both | Shell=WASM, content=VM |

### Edge Runtime: Mixed Execution

Edge workers CAN run pre-uploaded WASM. The constraint is they can't **dynamically instantiate** arbitrary WASM at runtime. This means:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            EDGE WORKER                                       │
│                                                                              │
│  Pre-deployed components ──► WASM (native execution)                        │
│       (Header, Footer, Layout)     ✓ Fast, pre-compiled                     │
│                                    ✓ Uploaded with worker                   │
│                                                                              │
│  User-defined content ──► Bytecode + VM (interpretation)                    │
│       (CMS pages, user templates)  ✓ Compiled on-demand                     │
│                                    ✓ Fetched from origin                    │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                    Single Page Render                                │    │
│  │                                                                      │    │
│  │   <Layout.wasm>           ◄── WASM (pre-deployed)                   │    │
│  │     <Header.wasm />       ◄── WASM (pre-deployed)                   │    │
│  │     <UserPage.motb>       ◄── VM interprets bytecode (dynamic)      │    │
│  │       <Button.wasm />     ◄── WASM (pre-deployed, called from VM)   │    │
│  │     </UserPage.motb>                                                 │    │
│  │     <Footer.wasm />       ◄── WASM (pre-deployed)                   │    │
│  │   </Layout.wasm>                                                     │    │
│  │                                                                      │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Browser Runtime: Native WASM for Everything

Browsers have no upload restriction—they can instantiate any WASM dynamically. This means:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              BROWSER                                         │
│                                                                              │
│  All components ──► WASM (native execution)                                 │
│                                                                              │
│  Pre-compiled WASM:     Downloaded from CDN, cached                         │
│  On-demand WASM:        Transpiled from bytecode at origin, streamed        │
│                                                                              │
│  No VM needed in browser—everything runs as native WASM.                    │
│  Source maps connect WASM execution back to .mot source.                    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Shared RPC Contract (Edge VM + Browser WASM)

Browser-side WASM modules must use the same logical RPC contract as edge VM execution.
The execution engine changes, but data/module lookup semantics do not.

- Data RPC: `POST /data/{page}` with `{ queryRef, signature, params, single }`
- Component/module lookup: same component path namespace used by edge runtime resolution
- Query validation: `queryRef + signature` validation remains mandatory

This keeps behavior consistent across:
- Edge VM execution
- Edge WASM execution (pre-deployed modules)
- Browser/editor WASM execution (reactive follow-up renders)

### Why Not Ship the VM to the Browser?

Shipping a VM/interpreter to the browser would mean:
1. Large download (the entire interpreter)
2. Interpretation overhead at runtime
3. No source map integration (debugging bytecode, not source)

Instead, user-defined content is **transpiled to WASM at the origin** before being sent to the browser:
1. Each component becomes a standalone WASM module
2. Browser's optimized WASM JIT executes directly
3. Source maps connect WASM → original .mot source
4. Small per-component modules, loaded on demand

### Execution Flow

```
1. Initial Page Load (Mixed Execution)
   ┌────────────┐     ┌─────────────────────────────────────┐     ┌─────────────┐
   │  Browser   │────►│           EDGE WORKER               │────►│   Origin    │
   │            │     │                                     │     │   Server    │
   └────────────┘     │  Pre-deployed WASM:                 │     └─────────────┘
         │            │    Layout.wasm ──► native exec      │            │
         │            │    Header.wasm ──► native exec      │            │
         │            │                                     │            │
         │            │  Dynamic content:                   │            │
         │            │    page.motb ◄────────────────────────── bytecode│
         │            │        │                            │            │
         │            │        ▼                            │            │
         │            │    VM interprets                    │            │
         │            │        │                            │            │
         │            │        ├── calls Button.wasm        │            │
         │            │        └── fetches data ────────────────► DB     │
         │            │                                     │            │
         │◄─── HTML stream (mixed from WASM + VM) ─────────┤            │
         │                                                  │            │
         │◄─── WASM modules (for browser reactivity) ──────┼────────────┤
         │                                                  │            │
         ▼                                                  │            │

2. Reactive Update (browser-side, all WASM)
   ┌────────────────────────────────────────────┐
   │                  Browser                    │
   │                                             │
   │  User Event ──► WASM Module ──► DOM Update  │
   │                      │                      │
   │                      ▼                      │
   │               Fetch new data ──────────────────► Origin
   │                      │                      │
   │                      ▼                      │
   │               Render fragment               │
   │                                             │
   │  All execution is native WASM—no VM needed  │
   └────────────────────────────────────────────┘
```

### Deployment Models

**Model A: Fully Pre-compiled (SaaS App)**
```
Build time:  All components ──► WASM modules
Deploy:      Upload WASM bundle to edge
Runtime:     Edge executes WASM only, no VM needed
Editor/Browser: Uses same WASM modules for preview and reactivity
```

**Model B: Fully Dynamic (CMS Platform)**
```
Build time:  System components ──► WASM modules
Runtime:     User pages ──► bytecode compiled on-demand
Edge:        WASM for system components, VM for user content
```

**Model C: Hybrid (E-commerce with User Templates)**
```
Build time:  App shell, product pages ──► WASM
Runtime:     Merchant custom pages ──► bytecode
Edge:        Mixed execution based on content source
```

### Compiler Architecture: Dual Targets

The compiler supports both targets as first-class outputs:

```
                              ┌─────────────────────────────────────┐
                              │           COMPILER                   │
                              │                                      │
  .mot source ──► Parser ──► AST ──► Analyzer ──┬──► Bytecode Emitter ──► .motb
                              │                 │                     │
                              │                 └──► WASM Generator ──► .wasm
                              │                                      │
                              │     Both paths share:                │
                              │     - AST representation             │
                              │     - Type checking                  │
                              │     - Dependency analysis            │
                              │     - Source map generation          │
                              └─────────────────────────────────────┘
```

**Compilation Modes:**

| Mode | Output | Use Case |
|------|--------|----------|
| `--target bytecode` | .motb | Dynamic content, on-demand compilation |
| `--target wasm` | .wasm | Pre-compiled components, static deployment |
| `--target both` | .motb + .wasm | Development (edge uses bytecode, browser uses WASM) |

#### 1. WASM Code Generation

New compiler phase that generates native WASM directly from AST (or from bytecode):

```
src/codegen/
├── wasm_gen.h         # WASM code generation API
├── wasm_gen.c         # AST → WASM translation
├── wasm_module.h      # WASM binary format
├── wasm_module.c      # Binary emission
├── wasm_optimize.c    # WASM-specific optimizations
└── sourcemap.c        # Source map generation
```

Each bytecode opcode has a WASM equivalent (for bytecode→WASM transpilation):

| Bytecode          | WASM Equivalent                        |
|-------------------|----------------------------------------|
| BC_CONST          | i32.const / f64.const + memory store   |
| BC_ADD            | i32.add / f64.add                      |
| BC_LOAD           | local.get + memory.load                |
| BC_EMIT_TEXT      | call $host_emit_text                   |
| BC_FETCH_DATA     | call $host_fetch_data (async import)   |
| BC_JUMP_IF_FALSE  | br_if                                  |
| BC_ITER_START     | loop + local variables                 |

#### 2. Source Map Support

Track source locations through every compilation phase:

```c
/* Extended AST node */
struct AstNode {
    NodeType type;
    int line;           /* Source line */
    int column;         /* Source column */
    const char *file;   /* Source file path (NEW) */
    // ...
};

/* Bytecode instruction with source info */
typedef struct {
    uint8_t opcode;
    uint32_t source_offset;  /* Index into source map */
} Instruction;

/* Source map entry */
typedef struct {
    uint32_t wasm_offset;    /* Byte offset in WASM */
    uint32_t source_line;
    uint32_t source_col;
    uint32_t name_index;     /* Index into names array */
} SourceMapEntry;
```

Output: Standard source map format (VLQ-encoded) that browser DevTools understand.

#### 3. Unified Host Interface

Both WASM modules and the VM share the **same host interface**. This enables:
- VM calling into WASM components
- WASM calling into VM-interpreted components
- Consistent behavior regardless of execution mode

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           HOST INTERFACE                                     │
│                                                                              │
│  ┌──────────────┐         ┌──────────────────────────┐                      │
│  │ WASM Module  │────────►│                          │                      │
│  └──────────────┘         │  emit_text(ptr, len)     │──► Output stream     │
│                           │  emit_raw(ptr, len)      │                      │
│  ┌──────────────┐         │  fetch_data(query_ref,   │──► Data provider     │
│  │ VM Bytecode  │────────►│             signature,   │                      │
│  └──────────────┘         │             params)       │                      │
│                           │  load_component(path)    │──► Component loader  │
│                           └──────────────────────────┘                      │
│                                                                              │
│  Component loader can return WASM or bytecode—caller doesn't care.          │
└─────────────────────────────────────────────────────────────────────────────┘
```

**WASM module structure:**

```wat
(module
  ;; Host imports (same interface as VM uses)
  (import "mot" "emit_text" (func $emit_text (param i32 i32)))
  (import "mot" "emit_raw" (func $emit_raw (param i32 i32)))
  ;; fetch_data(query_ref, signature, params_ptr) -> result_ptr
  (import "mot" "fetch_data" (func $fetch_data (param i32 i32 i32) (result i32)))
  (import "mot" "load_component" (func $load_component (param i32) (result i32)))

  ;; Memory shared with host
  (import "mot" "memory" (memory 1))

  ;; Component entry point
  (func (export "render") (param $props i32) (result i32)
    ;; Generated from AST or bytecode
  )
)
```

**Component loading with mixed execution:**

```javascript
// Edge worker: component loader that handles both WASM and bytecode
async function loadComponent(path) {
  // Check if pre-deployed WASM exists
  const wasmModule = predeployedModules.get(path);
  if (wasmModule) {
    return { type: 'wasm', module: wasmModule };
  }

  // Fall back to fetching bytecode from origin
  const bytecode = await fetchBytecode(originUrl, path);
  return { type: 'bytecode', data: bytecode };
}

// Unified render function
async function renderComponent(component, props) {
  if (component.type === 'wasm') {
    return await executeWasm(component.module, props);
  } else {
    return await vm.execute(component.data, props);
  }
}
```

### Browser Runtime

Minimal JavaScript runtime that:
1. Loads WASM modules on demand
2. Provides host function implementations
3. Manages DOM updates
4. Handles data fetching

```javascript
// browser-runtime/mot-runtime.js

class MotRuntime {
  constructor() {
    this.modules = new Map();      // path → WASM instance
    this.pending = new Map();      // path → Promise
  }

  async loadComponent(path) {
    if (this.modules.has(path)) {
      return this.modules.get(path);
    }

    if (!this.pending.has(path)) {
      this.pending.set(path, this.fetchAndInstantiate(path));
    }

    return this.pending.get(path);
  }

  async fetchAndInstantiate(path) {
    const response = await fetch(`/_mot/${path}.wasm`);
    const bytes = await response.arrayBuffer();

    const imports = {
      mot: {
        memory: this.memory,
        emit_text: (ptr, len) => this.emitText(ptr, len),
        emit_raw: (ptr, len) => this.emitRaw(ptr, len),
        fetch_data: (queryRef, signature, paramsPtr) =>
          this.fetchData(queryRef, signature, paramsPtr),
        load_component: (pathPtr) => this.loadNestedComponent(pathPtr),
      }
    };

    const { instance } = await WebAssembly.instantiate(bytes, imports);
    this.modules.set(path, instance);
    return instance;
  }

  // Render component to DOM element
  async renderTo(element, componentPath, props) {
    const instance = await this.loadComponent(componentPath);
    const propsPtr = this.writeProps(props);

    // Call WASM render function
    const htmlPtr = instance.exports.render(propsPtr);
    const html = this.readString(htmlPtr);

    // Efficient DOM update
    this.patchDOM(element, html);
  }
}
```

### Reactive Updates

Components can subscribe to data changes and re-render granularly:

```javascript
class ReactiveComponent {
  constructor(runtime, element, componentPath, props) {
    this.runtime = runtime;
    this.element = element;
    this.path = componentPath;
    this.props = props;
    this.subscriptions = [];
  }

  // Called when data dependency changes
  async invalidate(changedPaths) {
    // Check if any of our dependencies changed
    const affected = this.dependencies.some(dep =>
      changedPaths.some(path => path.startsWith(dep))
    );

    if (affected) {
      await this.render();
    }
  }

  async render() {
    const instance = await this.runtime.loadComponent(this.path);

    // WASM module tracks its own dependencies during render
    const result = instance.exports.render(this.propsPtr);

    // Get dependencies reported by WASM
    this.dependencies = this.runtime.getDependencies();

    // Patch DOM with new output
    this.runtime.patchDOM(this.element, result);
  }
}
```

### Data Flow for Reactivity

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                               BROWSER                                        │
│                                                                              │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                   │
│  │  Component A │    │  Component B │    │  Component C │                   │
│  │  (WASM)      │    │  (WASM)      │    │  (WASM)      │                   │
│  │              │    │              │    │              │                   │
│  │ deps: [      │    │ deps: [      │    │ deps: [      │                   │
│  │   user.name, │    │   cart.items │    │   user.name  │                   │
│  │   user.email │    │ ]            │    │ ]            │                   │
│  │ ]            │    │              │    │              │                   │
│  └──────────────┘    └──────────────┘    └──────────────┘                   │
│         │                   │                   │                            │
│         └───────────────────┴───────────────────┘                            │
│                             │                                                │
│                             ▼                                                │
│                    ┌─────────────────┐                                       │
│                    │  MotRuntime     │                                       │
│                    │                 │                                       │
│                    │  Data Store:    │◄──── WebSocket / SSE ◄──── Origin    │
│                    │  - user: {...}  │      (push updates)                   │
│                    │  - cart: {...}  │                                       │
│                    └─────────────────┘                                       │
│                             │                                                │
│                             │ user.name changes                              │
│                             ▼                                                │
│                    Notify Components A & C                                   │
│                    (Component B unaffected)                                  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Development DevTools

### Overview

In development mode, every page render includes instrumentation data that powers a debug sidebar. This reveals exactly what happened during rendering: modules loaded, data fetched, timing, and where each piece of HTML originated.

### Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Development Mode                                   │
│                                                                              │
│  ┌─────────────┐                                                            │
│  │   Origin    │──► Compile with debug info ──► Bytecode + Debug Metadata   │
│  │   Server    │                                                            │
│  └─────────────┘                                                            │
│         │                                                                    │
│         ▼                                                                    │
│  ┌─────────────┐                                                            │
│  │    Edge     │──► Instrument execution ──► Timing + Trace Data            │
│  │   Worker    │                                                            │
│  └─────────────┘                                                            │
│         │                                                                    │
│         ▼                                                                    │
│  ┌─────────────────────────────────────────────────────────────────────────┐ │
│  │                              Browser                                     │ │
│  │                                                                          │ │
│  │  ┌──────────────────────────────────────┐  ┌────────────────────────┐   │ │
│  │  │            Rendered Page             │  │     DevTools Sidebar   │   │ │
│  │  │                                      │  │                        │   │ │
│  │  │  ┌──────────────────────────────┐   │  │  ▸ Page: /products     │   │ │
│  │  │  │ <Header />                   │◄──┼──┼──  └─ render: 12ms     │   │ │
│  │  │  │  rendered from Header.mot    │   │  │                        │   │ │
│  │  │  └──────────────────────────────┘   │  │  ▸ Data Fetches        │   │ │
│  │  │                                      │  │    └─ products: 45ms  │   │ │
│  │  │  ┌──────────────────────────────┐   │  │    └─ categories: 23ms│   │ │
│  │  │  │ Product List                 │◄──┼──┼──                      │   │ │
│  │  │  │  5 items from SQL query      │   │  │  ▸ Components          │   │ │
│  │  │  └──────────────────────────────┘   │  │    └─ Header (dynamic) │   │ │
│  │  │                                      │  │    └─ ProductCard x5  │   │ │
│  │  └──────────────────────────────────────┘  │    └─ Footer (dynamic)│   │ │
│  │                                            │                        │   │ │
│  │                                            │  ▸ Dependencies        │   │ │
│  │                                            │    └─ products.*       │   │ │
│  │                                            │    └─ categories.*     │   │ │
│  │                                            └────────────────────────┘   │ │
│  └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Debug Metadata in Bytecode

When compiled in debug mode, bytecode includes:

```c
/* Debug info appended to bytecode module */
typedef struct {
    uint32_t source_file_count;
    SourceFile *source_files;    /* Array of source file paths */

    uint32_t span_count;
    DebugSpan *spans;            /* Instruction → source location mapping */

    uint32_t query_count;
    QueryInfo *queries;          /* SQL query metadata */

    uint32_t component_count;
    ComponentInfo *components;   /* Component import/render info */
} DebugInfo;

typedef struct {
    uint32_t start_pc;           /* Bytecode offset start */
    uint32_t end_pc;             /* Bytecode offset end */
    uint16_t file_index;         /* Index into source_files */
    uint16_t line;
    uint16_t column;
    uint8_t node_type;           /* AST node type for this span */
} DebugSpan;

typedef struct {
    uint32_t query_ref;          /* Query reference ID */
    uint16_t file_index;
    uint16_t line;
    char *sql_preview;           /* First 100 chars of SQL */
} QueryInfo;
```

### Runtime Instrumentation

Edge worker collects timing and trace data:

```javascript
// edge/src/debug-instrumentation.js

class RenderTrace {
  constructor() {
    this.spans = [];
    this.dataFetches = [];
    this.componentLoads = [];
    this.startTime = performance.now();
  }

  enterSpan(type, source) {
    return {
      type,
      source,
      startTime: performance.now(),
      children: [],
    };
  }

  exitSpan(span) {
    span.endTime = performance.now();
    span.duration = span.endTime - span.startTime;
    this.spans.push(span);
  }

  recordDataFetch(queryRef, signature, duration, rowCount) {
    this.dataFetches.push({
      queryRef,
      signature,
      duration,
      rowCount,
      timestamp: performance.now() - this.startTime,
    });
  }

  recordComponentLoad(path, dynamic, duration, bytecodeSize) {
    this.componentLoads.push({
      path,
      dynamic,
      duration,
      bytecodeSize,
      timestamp: performance.now() - this.startTime,
    });
  }

  toJSON() {
    return {
      totalDuration: performance.now() - this.startTime,
      spans: this.spans,
      dataFetches: this.dataFetches,
      componentLoads: this.componentLoads,
    };
  }
}
```

### DevTools Sidebar UI

Injected into the page in development mode:

```javascript
// browser-runtime/devtools.js

class MotDevTools {
  constructor(traceData) {
    this.trace = traceData;
    this.sidebar = this.createSidebar();
    this.overlay = this.createOverlay();
  }

  createSidebar() {
    const sidebar = document.createElement('div');
    sidebar.id = 'mot-devtools';
    sidebar.innerHTML = `
      <div class="mot-devtools-header">
        <h3>Motus DevTools</h3>
        <button class="mot-close">×</button>
      </div>
      <div class="mot-devtools-content">
        <section class="mot-section">
          <h4>Page Render</h4>
          <div class="mot-timing">
            Total: ${this.trace.totalDuration.toFixed(1)}ms
          </div>
        </section>

        <section class="mot-section">
          <h4>Data Fetches (${this.trace.dataFetches.length})</h4>
          <ul class="mot-list">
            ${this.trace.dataFetches.map(f => `
              <li>
                <span class="mot-query-ref">query:${f.queryRef}</span>
                <span class="mot-timing">${f.duration.toFixed(1)}ms</span>
                <span class="mot-count">${f.rowCount} rows</span>
              </li>
            `).join('')}
          </ul>
        </section>

        <section class="mot-section">
          <h4>Components (${this.trace.componentLoads.length})</h4>
          <ul class="mot-list">
            ${this.trace.componentLoads.map(c => `
              <li class="${c.dynamic ? 'mot-dynamic' : 'mot-static'}">
                <span class="mot-path">${c.path}</span>
                <span class="mot-badge">${c.dynamic ? 'dynamic' : 'static'}</span>
                <span class="mot-size">${(c.bytecodeSize / 1024).toFixed(1)}KB</span>
              </li>
            `).join('')}
          </ul>
        </section>

        <section class="mot-section">
          <h4>Dependencies</h4>
          <ul class="mot-list mot-deps">
            ${this.trace.dependencies?.map(d => `
              <li>${d}</li>
            `).join('') || '<li>None tracked</li>'}
          </ul>
        </section>
      </div>
    `;

    document.body.appendChild(sidebar);
    return sidebar;
  }

  // Highlight element origin on hover
  enableInspectMode() {
    document.addEventListener('mouseover', (e) => {
      const origin = e.target.closest('[data-mot-source]');
      if (origin) {
        this.showSourceOverlay(origin);
      }
    });
  }

  showSourceOverlay(element) {
    const source = element.dataset.motSource;
    const rect = element.getBoundingClientRect();

    this.overlay.style.top = `${rect.top}px`;
    this.overlay.style.left = `${rect.left}px`;
    this.overlay.style.width = `${rect.width}px`;
    this.overlay.style.height = `${rect.height}px`;
    this.overlay.querySelector('.mot-source-label').textContent = source;
    this.overlay.style.display = 'block';
  }
}
```

### Source Annotations in Output

In debug mode, rendered HTML includes source annotations:

```html
<!-- Debug mode output -->
<div data-mot-source="Header.mot:23" data-mot-component="Header">
  <header class="header">
    <h1 data-mot-source="Header.mot:25">My Store</h1>
  </header>
</div>

<div data-mot-source="index.mot:45" data-mot-query="products:q1">
  <!-- 5 items from: SELECT * FROM products WHERE ... -->
  <div data-mot-source="ProductCard.mot:12" data-mot-iter="0">
    ...
  </div>
</div>
```

### Trace Data Delivery

Debug data is delivered as a JSON blob embedded in the page:

```html
<!-- At end of page in development mode -->
<script id="mot-trace-data" type="application/json">
{
  "page": "index",
  "totalDuration": 127.4,
  "vm": "wasm",
  "bytecodeSize": 4523,
  "dataFetches": [
    {"queryRef": 1, "duration": 45.2, "rowCount": 5, "sql": "SELECT * FROM products..."},
    {"queryRef": 2, "duration": 23.1, "rowCount": 3, "sql": "SELECT * FROM categories..."}
  ],
  "componentLoads": [
    {"path": "components/Header", "dynamic": true, "duration": 12.3, "size": 1204},
    {"path": "components/ProductCard", "dynamic": false, "duration": 0, "size": 892}
  ],
  "dependencies": ["products.*", "categories.*"],
  "spans": [...]
}
</script>
<script src="/_mot/devtools.js"></script>
```

---

## Implementation Phases

### Phase 1: Debug Instrumentation (DevTools Foundation)
1. Add debug metadata to bytecode format
2. Instrument edge worker interpreter with timing
3. Create basic DevTools sidebar UI
4. Embed trace data in development responses

### Phase 2: Source Maps
1. Track source locations through compiler
2. Generate standard source map format
3. Connect bytecode spans to source
4. Enable browser DevTools source debugging

### Phase 3: WASM Code Generation
1. Define unified host interface (shared by VM and WASM)
2. Implement AST → WASM translation
3. Generate WASM binary format with source maps
4. Add `--target wasm` compiler flag

### Phase 4: Mixed Execution on Edge
1. Pre-deploy WASM bundle with edge worker
2. Component loader that resolves WASM or bytecode
3. VM can call into WASM components
4. Unified output streaming
5. Enforce shared RPC contract used by edge VM and browser WASM hosts

### Phase 5: Browser Runtime
1. WASM module loader (all components as WASM)
2. Host function implementations for browser
3. DOM patching utilities
4. On-demand WASM fetching
5. Reuse edge RPC paths/contracts for data and module resolution

### Phase 6: Reactive System
1. Dependency tracking in WASM execution
2. Subscription management
3. Granular re-rendering
4. Real-time data sync (WebSocket/SSE)

---

## File Structure for New Features

```
motus/
├── src/
│   ├── debug/
│   │   ├── debug.h              # Debug info structures
│   │   ├── debug.c              # Debug info emission
│   │   └── sourcemap.c          # Source map generation (VLQ encoding)
│   │
│   ├── codegen/
│   │   ├── ... (existing CSS/JS extraction)
│   │   ├── wasm_gen.h           # AST → WASM code generation
│   │   ├── wasm_gen.c
│   │   ├── wasm_module.h        # WASM binary format
│   │   ├── wasm_module.c
│   │   └── wasm_optimize.c      # WASM-specific optimizations
│   │
│   ├── host/                    # Unified host interface (shared by VM & WASM)
│   │   ├── host.h               # Host callback definitions
│   │   ├── host_edge.c          # Edge worker implementation
│   │   └── host_browser.c       # Browser implementation (compiled to WASM)
│   │
│   └── compiler/
│       └── ... (add --target flag support)
│
├── browser-runtime/             # Browser-side execution
│   ├── src/
│   │   ├── runtime.js           # WASM module loader & executor
│   │   ├── host.js              # Host function implementations
│   │   ├── reactive.js          # Dependency tracking & re-render
│   │   ├── dom.js               # Efficient DOM patching
│   │   ├── data.js              # Data fetching & caching
│   │   └── devtools.js          # DevTools sidebar UI
│   │
│   ├── styles/
│   │   └── devtools.css         # DevTools styling
│   │
│   └── build.js                 # Bundle for browser
│
├── example/
│   └── edge/
│       └── src/
│           ├── worker.js
│           ├── host.js              # Unified host implementation
│           ├── component-loader.js  # Mixed WASM/bytecode loading
│           └── debug-instrumentation.js
│
└── tests/
    ├── codegen/
    │   └── test_wasm_gen.c      # WASM generation tests
    ├── debug/
    │   └── test_sourcemap.c     # Source map tests
    ├── host/
    │   └── test_host.c          # Host interface tests
    └── browser-runtime/
        └── test_runtime.js      # Browser runtime tests
```
