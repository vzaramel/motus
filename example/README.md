# Motus Example: Edge Streaming

This example demonstrates Motus's edge rendering architecture:

```
Browser <---> Edge Worker <---> Origin Server
              (WASM)            (C Compiler)
```

## Architecture

### Origin Server (C)
- Precompiles configured pages/components at startup and keeps bytecode in memory
- Serves cached bytecode for precompiled routes, with on-demand fallback for others
- Streams bytecode to edge workers
- Serves component bytecode and data

### Edge Worker (Cloudflare Worker)
- Receives browser requests
- Fetches bytecode from origin (streaming)
- Interprets bytecode using WASM runtime
- Streams HTML to browser
- Loads components via origin `/component/...` (with edge cache reuse) and fetches data as needed

## Current Implementation Status

- Origin compiles/serves bytecode `1.2` and validates SQL RPC by `queryRef + signature`.
- Edge streams server-rendered HTML using WASM VM mode only (`wasm`).
- Browser bootstrap is currently disabled in this example while browser wasm/reactivity work is paused.
- Browser runtime fetches through same-origin edge proxies, not direct origin calls:
  - `/_mot/runtime/wasm`
  - `/_mot/page/{name}`
  - `/_mot/component/{name}`
  - `/_mot/data/{page}`
- Compiler/runtime reactivity supports:
  - direct path bindings (`@bind/@set/@plan`)
  - attribute path bindings (`@bindattr/@planattr`)
  - expression metadata (`@exprbind/@exprattr/@exprdep/@planexpr`)
- Edge builds per-page reactive wasm companion payloads from expression metadata.
- Browser runtime executes numeric-compatible expression updates via that per-page wasm module,
  with JS fallback for unsupported expression classes.

## Directory Structure

```
example/
├── content/
│   ├── pages/
│   │   └── index.mot        # Main page
│   └── components/
│       ├── Header.mot       # Header component
│       ├── ProductList.mot  # Product list with data
│       └── Footer.mot       # Footer component
├── origin/
│   ├── server.c             # C HTTP server
│   └── Makefile
├── edge/
│   ├── src/
│   │   └── worker.js        # Cloudflare Worker
│   ├── wrangler.toml        # Wrangler config
│   └── package.json
├── precompile-pages.txt     # Origin startup page precompile manifest
├── system-components.txt    # Shared/system component manifest
├── precompile-system-components.mjs
├── run.sh                   # Start everything
└── README.md
```

## Quick Start

### Prerequisites

- GCC (for origin server)
- Node.js and npm (for edge worker)
- Wrangler CLI (`npm install -g wrangler`)

### Running

1. **Build the Motus library:**
   ```bash
   cd /path/to/motus
   make lib
   ```

2. **Run the example:**
   ```bash
   cd example
   chmod +x run.sh
   ./run.sh
   ```

3. **Open in browser:**
   - Edge: http://localhost:8791
   - Origin: http://localhost:8080

### Runtime/Port Overrides

Use environment variables to control local ports and VM runtime:

```bash
cd example
ORIGIN_PORT=8081 EDGE_PORT=8788 MOT_VM=wasm MOT_BROWSER_WASM=0 ./run.sh
```

Supported VM modes:
- `MOT_VM=wasm` (default): use WASM VM and fail fast on unsupported bytecode/features.

Browser runtime mode:
- Browser wasm/reactivity is temporarily disabled across the stack.
- `MOT_BROWSER_WASM` and `?browser_wasm=1` are currently ignored.

### Shared System Components

System components listed in `example/system-components.txt` are precompiled as bundled edge artifacts:

- `example/edge/src/system-components.generated.js` (bytecode bundle)
- `example/edge/src/system-components.maps.generated.js` (source maps)

When origin emits `BC_COMPONENT_LINKED`, edge resolves from this bundle first (no origin fetch on hit).

Build/update the bundle:

```bash
cd example/edge
npm run build:system-components
```

### E2E Script (Wrangler)

Run a full local e2e check with explicit ports and VM mode:

```bash
cd example
./e2e-wrangler.sh --origin-port 8091 --edge-port 8792 --vm wasm --page wasm
```

Browser wasm bootstrap is currently disabled and should not be injected:

```bash
./e2e-wrangler.sh --origin-port 8091 --edge-port 8792 --vm wasm --page index
```

If the workspace has unrelated compile changes and you only want to verify runtime behavior with existing binaries:

```bash
./e2e-wrangler.sh --origin-port 8091 --edge-port 8792 --vm wasm --page index --skip-build
```

WASM target smoke checks across pages:

```bash
./target-matrix.sh --page index --origin-port-base 8200 --edge-port-base 8900
```

Matrix across fixture pages:

```bash
./target-matrix.sh --pages-file parity-pages.txt --origin-port-base 8200 --edge-port-base 8900
```

Repository parity test wrapper (used by CI):

```bash
cd ..
MOT_E2E=1 make test_wasm_parity
```

Contract snapshot parity checks (edge helpers vs runtime-wasm helpers):

```bash
cd edge
npm run test:contract-snapshot
```

Browser host adapter contract checks:

```bash
cd ..
node browser-runtime/test-host.mjs
```

Debug/trace validation (ensures `mot-trace-data`, sidebar bootstrap, `sourceMap`, and `executionPaths` payloads are present):

```bash
./e2e-debug.sh --mode trace --origin-port 8091 --edge-port 8798 --vm wasm --page layout
```

### Manual Setup

**Terminal 1 - Origin Server:**
```bash
cd example/origin
make
ORIGIN_PORT=8080 ./server
```

**Terminal 2 - Edge Worker:**
```bash
cd example/edge
npm install
npm run build:wasm
npx wrangler dev --port 8791 --var ORIGIN_URL:http://localhost:8080 --var MOT_VM:wasm
```

For debug symbols/source-level wasm debugging:
```bash
cd example/edge
npm install
npm run build:wasm:debug
npx wrangler dev --port 8791 --var ORIGIN_URL:http://localhost:8080 --var MOT_VM:wasm --var MOT_DEBUG:1
```

### Step Debugging (Chrome DevTools)

Use the dedicated runner (component mode is recommended):

```bash
node --inspect-brk runtime-wasm/scripts/step-debug.mjs \
  --source example/content/components/Footer.mot \
  --mode component \
  --function 0 \
  --props-json 'null' \
  --children-json 'null'
```

Then open `chrome://inspect`, attach to the Node target, and continue execution.

For linked pages/components, fetch prelinked bytecode from origin and debug that artifact:

```bash
curl -fsS http://127.0.0.1:8080/page/index > /tmp/index.bc
node --inspect-brk runtime-wasm/scripts/step-debug.mjs --bytecode /tmp/index.bc --mode page
```

## How It Works

1. **Browser** requests `http://localhost:8791/`

2. **Edge Worker** receives the request and:
   - Calls origin: `GET /page/index`
   - Receives bytecode stream

3. **Origin Server**:
   - Reads `content/pages/index.mot`
   - Compiles it to bytecode
   - Streams bytecode back

4. **Edge Worker** interprets bytecode:
   - Starts streaming HTML to browser
   - Encounters `<Header>` component → fetches `/component/Header`
   - Encounters `<ProductList>` → fetches `/component/ProductList`
   - Encounters SQL query → fetches `/data/products`

5. **Browser** receives HTML progressively

## API Endpoints

### Origin Server (port `ORIGIN_PORT`, default 8080)

| Endpoint | Description |
|----------|-------------|
| `GET /page/{name}` | Compile and stream page bytecode |
| `GET /component/{name}` | Compile and return component bytecode |
| `GET /runtime/wasm` | Serve browser runtime wasm module |
| `POST /data/{name}` | Execute SQL by query reference and return JSON data |
| `GET /health` | Health check |

Startup precompile behavior:
- Pages from `example/precompile-pages.txt` are compiled once at startup and retained in memory (bytecode + module metadata).
- Components from `example/system-components.txt` are compiled once at startup and retained as bytecode cache entries.

### Edge Worker (port `EDGE_PORT`, default 8791)

| Endpoint | Description |
|----------|-------------|
| `GET /` | Render index page |
| `GET /{page}` | Render specified page |
| `GET /health` | Health check |

## Example .mot Syntax

```xml
<!-- pages/index.mot -->
<html>
<body>
    <Header title="Welcome" />

    <let greeting "Hello from the Edge!">
        <h1><output greeting></h1>
    </let>

    <ProductList category="featured" />
</body>
</html>
```

```xml
<!-- components/ProductList.mot -->
<defcomp ProductList | category |>
    <style>
        .product { border: 1px solid #ddd; }
    </style>

    <let products dynamic select * from products where category eq category>
        <for product in products>
            <div class="product">
                <output product.name>
            </div>
        </for>
    </let>
</defcomp>
```

## Streaming Flow

```
Time →

Browser:  [Request] -------- [<html>] [<head>] [<body>] ... [</html>]
          ↓                    ↑         ↑        ↑           ↑
Edge:     [Fetch bytecode] → [Interpret] → → → → → → → → → →
          ↓                    ↑
Origin:   [Compile .mot] → [Stream bytecode]
```

The page renders progressively as:
1. Bytecode streams from origin to edge
2. Edge interprets and streams HTML to browser
3. Components/data are fetched on-demand

## Production Deployment

For production, deploy the edge worker to Cloudflare:

```bash
cd edge
npx wrangler deploy
```

Configure `ORIGIN_URL` in `wrangler.toml` to point to your origin server.
