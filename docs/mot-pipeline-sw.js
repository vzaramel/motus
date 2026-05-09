/*
 * Motus Pipeline Simulation — Service Worker
 *
 * Simulates the full Origin → Edge → Browser pipeline:
 *   - sql.js (SQLite) as the database backend
 *   - Motus WASM for compilation + rendering
 *   - Suspend/resume for data fetching
 *   - Component compilation + caching
 *   - Streaming HTML output
 *
 * Endpoints:
 *   POST /__mot-sim/compile   →  Origin: source → bytecode + CSS
 *   POST /__mot-sim/render    →  Edge: bytecode → streamed HTML (with data fetching)
 *   POST /__mot-sim/mutate    →  Origin: insert row → invalidate → re-render
 *   POST /__mot-sim/reset     →  Clear all caches
 */

/* ===== Load sql.js at top level (runs on every SW evaluation, not just install) ===== */
try {
    importScripts('https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.11.0/sql-wasm.js');
} catch (e) {
    try {
        importScripts('https://cdn.jsdelivr.net/npm/sql.js@1.11.0/dist/sql-wasm.js');
    } catch (e2) {
        console.error('[mot-sw] Failed to load sql.js:', e2);
    }
}

/* ===== Global State ===== */
var compileInstance = null;  /* WASM for compilation */
var renderInstance = null;   /* WASM for rendering (suspend/resume) */
var compileReady = null;
var renderReady = null;
var db = null;
var dbReady = null;

/* Caches */
var bytecodeCache = new Map();
var dataCache = new Map();
var cacheStats = { bcHit: 0, bcMiss: 0, dataHit: 0, dataMiss: 0 };

/* Pending request captured from host callbacks */
var pendingFetch = null;
var pendingComponent = null;
var renderHtmlBuf = '';
var compileHtmlBuf = '';

/* Component registry: path → source code */
var componentRegistry = {};

/* ===== Utilities ===== */

function findBaseUrl() {
    return self.location.pathname.replace('mot-pipeline-sw.js', '');
}

function sleep(ms) {
    return new Promise(function (r) { setTimeout(r, ms); });
}

function hashString(s) {
    var h = 0;
    for (var i = 0; i < s.length; i++) {
        h = ((h << 5) - h + s.charCodeAt(i)) | 0;
    }
    return h.toString(36);
}

function writeString(instance, text) {
    var enc = new TextEncoder().encode(text || '');
    var ptr = instance.exports.mot_alloc(enc.length + 1);
    if (!ptr) throw new Error('mot_alloc failed');
    new Uint8Array(instance.exports.memory.buffer, ptr, enc.length).set(enc);
    new Uint8Array(instance.exports.memory.buffer)[ptr + enc.length] = 0;
    return { ptr: ptr, len: enc.length };
}

function readString(memory, ptr, len) {
    if (!ptr || !len) return '';
    return new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len));
}

function readCString(memory, ptr, maxLen) {
    if (!ptr) return '';
    var mem = new Uint8Array(memory.buffer);
    if (ptr < 0 || ptr >= mem.length) return '';
    var end = ptr;
    var limit = Math.min(mem.length, ptr + (maxLen || 4096));
    while (end < limit && mem[end] !== 0) end++;
    return new TextDecoder().decode(mem.subarray(ptr, end));
}

/* ===== sql.js Database ===== */

function ensureDb() {
    if (dbReady) return dbReady;
    dbReady = (async function () {
        var SQL = await initSqlJs({
            locateFile: function (f) {
                return 'https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.11.0/' + f;
            }
        });
        db = new SQL.Database();
        seedDatabase();
        return db;
    })();
    return dbReady;
}

function seedDatabase() {
    db.run('CREATE TABLE contacts (id INTEGER PRIMARY KEY, name TEXT, email TEXT, company TEXT)');
    db.run('CREATE TABLE orders (id INTEGER PRIMARY KEY, contact_id INTEGER, product TEXT, amount REAL, status TEXT)');

    db.run("INSERT INTO contacts VALUES (1, 'Alice Chen', 'alice@acme.co', 'Acme Corp')");
    db.run("INSERT INTO contacts VALUES (2, 'Bob Martinez', 'bob@globex.io', 'Globex Inc')");
    db.run("INSERT INTO contacts VALUES (3, 'Carol Wu', 'carol@initech.com', 'Initech')");
    db.run("INSERT INTO contacts VALUES (4, 'Dan Okafor', 'dan@hooli.dev', 'Hooli')");
    db.run("INSERT INTO contacts VALUES (5, 'Eva Lindgren', 'eva@piedpiper.net', 'Pied Piper')");

    db.run("INSERT INTO orders VALUES (1, 1, 'Widget Pro', 299.99, 'shipped')");
    db.run("INSERT INTO orders VALUES (2, 1, 'Gadget X', 149.50, 'delivered')");
    db.run("INSERT INTO orders VALUES (3, 1, 'Bolt Pack 100', 45.00, 'processing')");
    db.run("INSERT INTO orders VALUES (4, 2, 'Widget Pro', 299.99, 'delivered')");
    db.run("INSERT INTO orders VALUES (5, 2, 'Nano Sensor', 89.00, 'shipped')");
    db.run("INSERT INTO orders VALUES (6, 3, 'Gadget X', 149.50, 'shipped')");
    db.run("INSERT INTO orders VALUES (7, 3, 'Bolt Pack 100', 45.00, 'delivered')");
    db.run("INSERT INTO orders VALUES (8, 3, 'Mega Drive', 599.00, 'processing')");
    db.run("INSERT INTO orders VALUES (9, 4, 'Nano Sensor', 89.00, 'delivered')");
    db.run("INSERT INTO orders VALUES (10, 4, 'Widget Pro', 299.99, 'processing')");
    db.run("INSERT INTO orders VALUES (11, 5, 'Gadget X', 149.50, 'shipped')");
    db.run("INSERT INTO orders VALUES (12, 5, 'Mega Drive', 599.00, 'delivered')");
    db.run("INSERT INTO orders VALUES (13, 5, 'Bolt Pack 100', 45.00, 'shipped')");
}

function runQuery(sql, params) {
    if (!db) throw new Error('Database not loaded');
    var stmt = db.prepare(sql);
    if (params) stmt.bind(params);
    var rows = [];
    while (stmt.step()) rows.push(stmt.getAsObject());
    stmt.free();
    return rows;
}

/* ===== WASM Loading ===== */

function createWasmInstance(label) {
    var htmlBuf = '';
    var fetchCb = null;
    var componentCb = null;
    var memory = null;

    var imports = {
        env: {
            host_output: function (ptr, len) {
                htmlBuf += readString({ buffer: memory.buffer }, ptr, len);
            },
            host_output_text: function (ptr, len) {
                var t = readString({ buffer: memory.buffer }, ptr, len);
                htmlBuf += t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
            },
            host_error: function (ptr, len) {
                console.error('[mot-sw:' + label + ']', readString({ buffer: memory.buffer }, ptr, len));
            },
            host_log: function () {},
            host_render_complete: function () {},
            host_dep_start: function () {},
            host_dep_end: function () {},
            host_component_start: function () {},
            host_component_end: function () {},
            host_slot_default_start: function () {},
            host_slot_default_end: function () {},
            host_debug_step: function () {},
            host_fetch_data: function (reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
                if (fetchCb) fetchCb(reqId, queryRef, signature, namePtr, paramsPtr, isSingle);
            },
            host_load_component: function (reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
                if (componentCb) componentCb(reqId, namePtr, pathPtr, propsPtr, childrenPtr);
            },
            host_load_component_linked: function (reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
                if (componentCb) componentCb(reqId, namePtr, pathPtr, propsPtr, childrenPtr);
            }
        }
    };

    return (async function () {
        var url = findBaseUrl() + 'mot-runtime.wasm';
        var result;
        try {
            result = await WebAssembly.instantiateStreaming(fetch(url), imports);
        } catch (e) {
            var resp = await fetch(url);
            var buf = await resp.arrayBuffer();
            result = await WebAssembly.instantiate(buf, imports);
        }
        var ex = result.instance.exports;
        memory = ex.memory;
        return {
            memory: ex.memory,
            exports: ex,
            getHtml: function () { return htmlBuf; },
            resetHtml: function () { htmlBuf = ''; },
            setFetchCallback: function (fn) { fetchCb = fn; },
            setComponentCallback: function (fn) { componentCb = fn; }
        };
    })();
}

function ensureCompileInstance() {
    if (compileReady) return compileReady;
    compileReady = createWasmInstance('compile').then(function (inst) {
        compileInstance = inst;
        return inst;
    });
    return compileReady;
}

function ensureRenderInstance() {
    if (renderReady) return renderReady;
    renderReady = createWasmInstance('render').then(function (inst) {
        renderInstance = inst;
        return inst;
    });
    return renderReady;
}

/* ===== Compilation ===== */

function compileMot(instance, source) {
    var ex = instance.exports;
    ex.mot_reset_alloc();
    var enc = new TextEncoder().encode(source);
    var ptr = ex.mot_alloc(enc.length + 1);
    if (!ptr) return { error: 'Alloc failed' };
    new Uint8Array(ex.memory.buffer, ptr, enc.length).set(enc);
    new Uint8Array(ex.memory.buffer)[ptr + enc.length] = 0;

    var rc = ex.mot_wasm_compile(ptr, enc.length);
    if (rc !== 0) {
        var ePtr = ex.mot_wasm_get_error_ptr();
        var eLen = ex.mot_wasm_get_error_len();
        return { error: eLen > 0 ? readString(ex.memory, ePtr, eLen) : 'Compilation error' };
    }

    var bcPtr = ex.mot_wasm_get_bytecode_ptr();
    var bcLen = ex.mot_wasm_get_bytecode_len();
    var bytecode = new Uint8Array(ex.memory.buffer, bcPtr, bcLen).slice();

    var cssPtr = ex.mot_wasm_get_css_ptr();
    var cssLen = ex.mot_wasm_get_css_len();
    var css = cssLen > 0 ? readString(ex.memory, cssPtr, cssLen) : '';

    return { bytecode: bytecode, css: css };
}

/* ===== SW Events ===== */

self.addEventListener('install', function (event) {
    self.skipWaiting();
});

self.addEventListener('activate', function (event) {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', function (event) {
    var url = new URL(event.request.url);
    if (url.pathname.endsWith('/__mot-sim/compile')) {
        event.respondWith(handleCompile(event.request));
    } else if (url.pathname.endsWith('/__mot-sim/render')) {
        event.respondWith(handleRender(event.request));
    } else if (url.pathname.endsWith('/__mot-sim/mutate')) {
        event.respondWith(handleMutate(event.request));
    } else if (url.pathname.endsWith('/__mot-sim/reset')) {
        event.respondWith(handleReset());
    }
});

/* ===== Handlers ===== */

async function handleReset() {
    bytecodeCache.clear();
    dataCache.clear();
    cacheStats = { bcHit: 0, bcMiss: 0, dataHit: 0, dataMiss: 0 };
    return new Response(JSON.stringify({ ok: true }), { headers: { 'Content-Type': 'application/json' } });
}

async function handleCompile(request) {
    try {
        await ensureCompileInstance();
    } catch (e) {
        return new Response(JSON.stringify({ error: 'WASM load failed: ' + e.message }), { status: 503, headers: { 'Content-Type': 'application/json' } });
    }

    var latency = parseInt(request.headers.get('X-Sim-Latency-Ms') || '0');
    var body = await request.json();
    var source = body.source || '';
    var components = body.components || {};

    /* Register components for the render phase */
    Object.keys(components).forEach(function (k) { componentRegistry[k] = components[k]; });

    if (latency > 0) await sleep(latency);

    /* Check bytecode cache */
    var cacheKey = hashString(source);
    var cached = bytecodeCache.get(cacheKey);
    if (cached) {
        cacheStats.bcHit++;
        var buf = packCompileResponse(cached.bytecode, cached.css);
        return new Response(buf, {
            headers: { 'Content-Type': 'application/octet-stream', 'X-Cache': 'HIT' }
        });
    }
    cacheStats.bcMiss++;

    var result = compileMot(compileInstance, source);
    if (result.error) {
        return new Response(JSON.stringify({ error: result.error }), {
            status: 400, headers: { 'Content-Type': 'application/json' }
        });
    }

    bytecodeCache.set(cacheKey, { bytecode: result.bytecode, css: result.css });

    var buf = packCompileResponse(result.bytecode, result.css);
    return new Response(buf, {
        headers: { 'Content-Type': 'application/octet-stream', 'X-Cache': 'MISS' }
    });
}

function packCompileResponse(bytecode, css) {
    var cssBytes = css ? new TextEncoder().encode(css) : new Uint8Array(0);
    var buf = new ArrayBuffer(4 + bytecode.length + cssBytes.length);
    new DataView(buf).setUint32(0, bytecode.length, true);
    new Uint8Array(buf, 4, bytecode.length).set(bytecode);
    if (cssBytes.length > 0) new Uint8Array(buf, 4 + bytecode.length).set(cssBytes);
    return buf;
}

/* Reusable render loop: loads bytecode, runs suspend/resume, returns HTML + events */
async function renderWithBytecode(bytecode, events, latency) {
    var ex = renderInstance.exports;
    renderInstance.resetHtml();
    ex.mot_reset_alloc();

    var ptr = ex.mot_alloc(bytecode.length);
    if (!ptr) throw new Error('Alloc failed');
    new Uint8Array(ex.memory.buffer, ptr, bytecode.length).set(bytecode);

    var rc = ex.mot_init(ptr, bytecode.length);
    if (rc !== 0) throw new Error('VM init failed');

    var pendingData = null;
    renderInstance.setFetchCallback(function (reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
        var name = readCString(ex.memory, namePtr, 256);
        var paramsJson = readCString(ex.memory, paramsPtr, 4096);
        pendingData = { reqId: reqId, queryRef: queryRef, signature: signature, name: name, paramsJson: paramsJson, isSingle: isSingle };
    });

    var pendingComp = null;
    renderInstance.setComponentCallback(function (reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
        var name = readCString(ex.memory, namePtr, 256);
        var path = readCString(ex.memory, pathPtr, 512);
        var propsJson = readCString(ex.memory, propsPtr, 4096);
        var childrenJson = readCString(ex.memory, childrenPtr, 4096);
        pendingComp = { reqId: reqId, name: name, path: path, propsJson: propsJson, childrenJson: childrenJson };
    });

    var t0 = performance.now();

    rc = ex.mot_render();
    while (rc === -2) {
        if (pendingData) {
            var pd = pendingData;
            pendingData = null;

            var dataCacheKey = pd.name + ':' + pd.paramsJson;
            var cachedData = dataCache.get(dataCacheKey);
            var dataResult;
            var cacheHit = false;

            if (cachedData) {
                dataResult = cachedData;
                cacheHit = true;
                cacheStats.dataHit++;
            } else {
                dataResult = resolveDataQuery(pd.name, pd.paramsJson);
                dataCache.set(dataCacheKey, dataResult);
                cacheStats.dataMiss++;
            }

            if (latency > 0) await sleep(Math.floor(latency / 2));

            events.push({ type: 'data', name: pd.name, params: pd.paramsJson, cache: cacheHit, time: performance.now() - t0 });

            var jsonStr = JSON.stringify(pd.isSingle ? (dataResult[0] || null) : dataResult);
            var ws = writeString(renderInstance, jsonStr);
            rc = ex.mot_resume(pd.reqId, ws.ptr, ws.len);

        } else if (pendingComp) {
            var pc = pendingComp;
            pendingComp = null;

            var compHtml = await resolveComponent(pc);

            if (latency > 0) await sleep(Math.floor(latency / 2));

            events.push({ type: 'component', name: pc.name, path: pc.path, time: performance.now() - t0 });

            var ws2 = writeString(renderInstance, compHtml);
            rc = ex.mot_resume(pc.reqId, ws2.ptr, ws2.len);

        } else {
            break;
        }
    }

    renderInstance.setFetchCallback(null);
    renderInstance.setComponentCallback(null);

    if (rc !== 0 && rc !== -2) throw new Error('Render failed (code ' + rc + ')');

    return renderInstance.getHtml();
}

async function handleRender(request) {
    try {
        await Promise.all([ensureRenderInstance(), ensureCompileInstance(), ensureDb()]);
    } catch (e) {
        return new Response(JSON.stringify({ error: 'Init failed: ' + e.message }), { status: 503, headers: { 'Content-Type': 'application/json' } });
    }

    var latency = parseInt(request.headers.get('X-Sim-Latency-Ms') || '0');
    var chunkSize = parseInt(request.headers.get('X-Sim-Chunk-Size') || '256');
    var bytecode = new Uint8Array(await request.arrayBuffer());

    if (latency > 0) await sleep(latency);

    var events = [];
    var fullHtml;
    try {
        fullHtml = await renderWithBytecode(bytecode, events, latency);
    } catch (e) {
        return new Response(JSON.stringify({ error: e.message }), { status: 500, headers: { 'Content-Type': 'application/json' } });
    }

    /* Stream response with timing events in header */
    var chunks = [];
    for (var i = 0; i < fullHtml.length; i += chunkSize) {
        chunks.push(fullHtml.slice(i, i + chunkSize));
    }
    if (chunks.length === 0) chunks.push('');

    var chunkDelay = Math.max(15, Math.floor(latency / 4));
    var encoder = new TextEncoder();

    var stream = new ReadableStream({
        start: function (controller) {
            var idx = 0;
            function pushNext() {
                if (idx >= chunks.length) { controller.close(); return; }
                controller.enqueue(encoder.encode(chunks[idx++]));
                setTimeout(pushNext, chunkDelay);
            }
            pushNext();
        }
    });

    return new Response(stream, {
        headers: {
            'Content-Type': 'text/html; charset=utf-8',
            'X-Pipeline-Events': JSON.stringify(events),
            'X-Cache-Stats': JSON.stringify(cacheStats)
        }
    });
}

/* ===== Mutation Handler ===== */

async function handleMutate(request) {
    try {
        await Promise.all([ensureRenderInstance(), ensureCompileInstance(), ensureDb()]);
    } catch (e) {
        return errorResponse('Init failed: ' + e.message, 503);
    }

    var body = await request.json();
    var table = body.table;
    var row = body.row;
    var latency = parseInt(request.headers.get('X-Sim-Latency-Ms') || '0');

    /* 1. Insert into database */
    var newId = 0;
    if (table === 'contacts') {
        var maxId = runQuery('SELECT MAX(id) as m FROM contacts');
        newId = ((maxId[0] && maxId[0].m) || 0) + 1;
        db.run("INSERT INTO contacts VALUES (?, ?, ?, ?)", [newId, row.name || '', row.email || '', row.company || '']);
    } else if (table === 'orders') {
        var maxOrd = runQuery('SELECT MAX(id) as m FROM orders');
        newId = ((maxOrd[0] && maxOrd[0].m) || 0) + 1;
        db.run("INSERT INTO orders VALUES (?, ?, ?, ?, ?)", [newId, row.contact_id || 0, row.product || '', row.amount || 0, row.status || 'processing']);
    } else {
        return errorResponse('Unknown table: ' + table, 400);
    }

    if (latency > 0) await sleep(Math.floor(latency / 2));

    /* 2. Invalidate data cache for this table */
    dataCache.forEach(function (v, k) {
        if (k.indexOf(table) === 0) dataCache.delete(k);
    });

    /* 3. Find cached bytecode (from the last compile) */
    var lastBytecode = null;
    var lastCss = '';
    bytecodeCache.forEach(function (v) { lastBytecode = v.bytecode; lastCss = v.css; });

    if (!lastBytecode) {
        return errorResponse('No cached bytecode — run the pipeline first', 400);
    }

    /* 4. Re-render with cached bytecode + fresh data */
    var events = [];
    var html;
    try {
        html = await renderWithBytecode(lastBytecode, events, latency);
    } catch (e) {
        return errorResponse('Re-render failed: ' + e.message, 500);
    }

    /* 5. Return full HTML + invalidation info */
    return new Response(JSON.stringify({
        html: html,
        css: lastCss,
        invalidated: [table],
        events: events,
        newRow: Object.assign({ id: newId }, row)
    }), {
        headers: {
            'Content-Type': 'application/json',
            'X-Mot-Invalidated': table,
            'X-Cache-Stats': JSON.stringify(cacheStats)
        }
    });
}

function errorResponse(msg, status) {
    return new Response(JSON.stringify({ error: msg }), {
        status: status || 500,
        headers: { 'Content-Type': 'application/json' }
    });
}

/* ===== Data Query Resolution ===== */

function resolveDataQuery(name, paramsJson) {
    var params = {};
    try { params = JSON.parse(paramsJson || '{}'); } catch (e) {}

    /* Map binding name to SQL query.
       In the real system, the origin has the SQL from the bytecode module.
       Here we use a simple name-based mapping. */
    if (name === 'contacts') {
        return runQuery('SELECT * FROM contacts ORDER BY id');
    }
    if (name === 'orders') {
        /* Check for contact_id filter in params */
        var contactId = params.contact_id || params['contact.id'] || null;
        if (contactId) {
            return runQuery('SELECT * FROM orders WHERE contact_id = ? ORDER BY id', [contactId]);
        }
        return runQuery('SELECT * FROM orders ORDER BY id');
    }
    /* Fallback: try generic query */
    return [];
}

/* ===== Component Resolution ===== */

async function resolveComponent(pending) {
    var source = componentRegistry[pending.path] || componentRegistry[pending.name] || null;
    if (!source) return '<div style="color:red">Component not found: ' + pending.name + '</div>';

    /* Compile the component */
    var compiled = compileMot(compileInstance, source);
    if (compiled.error) return '<div style="color:red">Compile error: ' + compiled.error + '</div>';

    /* Render with props */
    /* For the demo, we compile a wrapper that instantiates the component with props.
       This is a simplification — the real system passes props via stack. */
    var propsObj = {};
    try { propsObj = JSON.parse(pending.propsJson || '{}'); } catch (e) {}

    /* Build a wrapper source that calls the component with the props */
    var wrapperParts = [source, '\n'];
    var propAttrs = Object.keys(propsObj).map(function (k) {
        var v = propsObj[k];
        if (typeof v === 'string') return k + '="' + v.replace(/"/g, '&quot;') + '"';
        if (typeof v === 'object' && v !== null) {
            /* For object props, we need to create let bindings */
            return '';
        }
        return k + '="' + String(v) + '"';
    }).filter(Boolean).join(' ');

    /* For object props (like contact), create let bindings from the object fields */
    var letBindings = '';
    Object.keys(propsObj).forEach(function (k) {
        var v = propsObj[k];
        if (typeof v === 'object' && v !== null) {
            /* Create a JSON-like initialization. Since we can't pass objects directly in the template,
               we'll create a simple rendering by substituting values inline. */
            /* Actually, let's just render the component source with string replacements
               for the prop references. This is the pragmatic approach for the demo. */
        }
    });

    /* Pragmatic approach: render the component source directly, replacing prop references
       with actual values. This works for simple cases. */
    var renderedSource = renderComponentWithProps(source, propsObj);
    var innerCompiled = compileMot(compileInstance, renderedSource);
    if (innerCompiled.error) return '<div style="color:red">' + innerCompiled.error + '</div>';

    /* Use a separate render pass for the component */
    compileInstance.resetHtml();
    var cex = compileInstance.exports;
    cex.mot_reset_alloc();
    var bcPtr = cex.mot_alloc(innerCompiled.bytecode.length);
    new Uint8Array(cex.memory.buffer, bcPtr, innerCompiled.bytecode.length).set(innerCompiled.bytecode);
    var initRc = cex.mot_init(bcPtr, innerCompiled.bytecode.length);
    if (initRc !== 0) return '<div style="color:red">Component init failed</div>';

    /* Component render also needs data fetching */
    var compPendingData = null;
    compileInstance.setFetchCallback(function (reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
        var name = readCString(cex.memory, namePtr, 256);
        var paramsJsonStr = readCString(cex.memory, paramsPtr, 4096);
        compPendingData = { reqId: reqId, name: name, paramsJson: paramsJsonStr, isSingle: isSingle };
    });

    var renderRc = cex.mot_render();
    while (renderRc === -2 && compPendingData) {
        var cpd = compPendingData;
        compPendingData = null;

        var dataResult = resolveDataQuery(cpd.name, cpd.paramsJson);
        var jsonStr = JSON.stringify(cpd.isSingle ? (dataResult[0] || null) : dataResult);
        var ws = writeString(compileInstance, jsonStr);
        renderRc = cex.mot_resume(cpd.reqId, ws.ptr, ws.len);
    }

    compileInstance.setFetchCallback(null);

    var html = compileInstance.getHtml();
    if (innerCompiled.css) html = '<style>' + innerCompiled.css + '</style>' + html;
    return html;
}

function renderComponentWithProps(source, props) {
    /* Strip the <defcomp> wrapper and replace prop references with values.
       This is a simplified approach for the demo. */
    var inner = source;

    /* Remove <defcomp ...> opening and </defcomp> closing */
    inner = inner.replace(/<defcomp\s+\w+[^>]*>/, '');
    inner = inner.replace(/<\/defcomp>/, '');

    /* Replace <output propName.field> patterns */
    Object.keys(props).forEach(function (propName) {
        var val = props[propName];
        if (typeof val === 'object' && val !== null) {
            Object.keys(val).forEach(function (field) {
                var re = new RegExp('<output\\s+' + propName + '\\.' + field + '\\s*>', 'g');
                inner = inner.replace(re, String(val[field] || ''));
            });
            /* Also replace bare prop references used in query params */
            var reParam = new RegExp('=\\s*' + propName + '\\.(\\w+)', 'g');
            inner = inner.replace(reParam, function (m, f) {
                return '= ' + JSON.stringify(val[f] !== undefined ? val[f] : '');
            });
        } else {
            var re2 = new RegExp('<output\\s+' + propName + '\\s*>', 'g');
            inner = inner.replace(re2, String(val || ''));
        }
    });

    return inner.trim();
}

