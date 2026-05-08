/*
 * Motus Pipeline Simulation — Service Worker
 *
 * Simulates the Origin → Edge pipeline by intercepting synthetic fetch
 * requests and processing them with the WASM compiler + VM.
 *
 *   POST /__mot-sim/compile  →  "Origin" compiles source to bytecode
 *   POST /__mot-sim/render   →  "Edge" renders bytecode to streamed HTML
 */

var wasmInstance = null;
var htmlBuf = '';
var wasmReady = null;

function findWasmUrl() {
    return self.location.pathname.replace('mot-pipeline-sw.js', 'mot-runtime.wasm');
}

function sleep(ms) {
    return new Promise(function (r) { setTimeout(r, ms); });
}

function ensureWasm() {
    if (wasmReady) return wasmReady;
    wasmReady = (async function () {
        var memory = null;
        var imports = {
            env: {
                host_output: function (ptr, len) {
                    var bytes = new Uint8Array(memory.buffer, ptr, len);
                    htmlBuf += new TextDecoder().decode(bytes);
                },
                host_output_text: function (ptr, len) {
                    var bytes = new Uint8Array(memory.buffer, ptr, len);
                    var t = new TextDecoder().decode(bytes);
                    htmlBuf += t.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
                },
                host_error: function () {},
                host_log: function () {},
                host_render_complete: function () {},
                host_dep_start: function () {},
                host_dep_end: function () {},
                host_component_start: function () {},
                host_component_end: function () {},
                host_slot_default_start: function () {},
                host_slot_default_end: function () {},
                host_debug_step: function () {},
                host_fetch_data: function () {},
                host_load_component: function () {},
                host_load_component_linked: function () {}
            }
        };

        var url = findWasmUrl();
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
        wasmInstance = { memory: ex.memory, exports: ex };
    })();
    return wasmReady;
}

self.addEventListener('install', function (event) {
    self.skipWaiting();
    event.waitUntil(ensureWasm());
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
    }
});

async function handleCompile(request) {
    try { await ensureWasm(); } catch (e) {
        return new Response(JSON.stringify({ error: 'WASM load failed: ' + e.message }), { status: 503, headers: { 'Content-Type': 'application/json' } });
    }

    var latency = parseInt(request.headers.get('X-Sim-Latency-Ms') || '0');
    var source = await request.text();

    if (latency > 0) await sleep(latency);

    var ex = wasmInstance.exports;
    ex.mot_reset_alloc();

    var enc = new TextEncoder().encode(source);
    var ptr = ex.mot_alloc(enc.length + 1);
    if (!ptr) return new Response('Alloc failed', { status: 500 });
    new Uint8Array(ex.memory.buffer, ptr, enc.length).set(enc);
    new Uint8Array(ex.memory.buffer)[ptr + enc.length] = 0;

    var rc = ex.mot_wasm_compile(ptr, enc.length);
    if (rc !== 0) {
        var ePtr = ex.mot_wasm_get_error_ptr();
        var eLen = ex.mot_wasm_get_error_len();
        var err = eLen > 0
            ? new TextDecoder().decode(new Uint8Array(ex.memory.buffer, ePtr, eLen))
            : 'Unknown error';
        return new Response(JSON.stringify({ error: err }), {
            status: 400,
            headers: { 'Content-Type': 'application/json' }
        });
    }

    var bcPtr = ex.mot_wasm_get_bytecode_ptr();
    var bcLen = ex.mot_wasm_get_bytecode_len();
    var bytecode = new Uint8Array(ex.memory.buffer, bcPtr, bcLen).slice();

    var cssPtr = ex.mot_wasm_get_css_ptr();
    var cssLen = ex.mot_wasm_get_css_len();
    var cssBytes = cssLen > 0
        ? new Uint8Array(ex.memory.buffer, cssPtr, cssLen).slice()
        : new Uint8Array(0);

    /* Response format: [u32 bc_len LE][bytecode][css] */
    var buf = new ArrayBuffer(4 + bytecode.length + cssBytes.length);
    new DataView(buf).setUint32(0, bytecode.length, true);
    new Uint8Array(buf, 4, bytecode.length).set(bytecode);
    if (cssBytes.length > 0) {
        new Uint8Array(buf, 4 + bytecode.length, cssBytes.length).set(cssBytes);
    }

    return new Response(buf, {
        headers: { 'Content-Type': 'application/octet-stream' }
    });
}

async function handleRender(request) {
    try { await ensureWasm(); } catch (e) {
        return new Response('WASM load failed: ' + e.message, { status: 503 });
    }

    var latency = parseInt(request.headers.get('X-Sim-Latency-Ms') || '0');
    var chunkSize = parseInt(request.headers.get('X-Sim-Chunk-Size') || '256');
    var bytecode = new Uint8Array(await request.arrayBuffer());

    if (latency > 0) await sleep(latency);

    var ex = wasmInstance.exports;
    htmlBuf = '';
    ex.mot_reset_alloc();

    var ptr = ex.mot_alloc(bytecode.length);
    if (!ptr) return new Response('Alloc failed', { status: 500 });
    new Uint8Array(ex.memory.buffer, ptr, bytecode.length).set(bytecode);

    var rc = ex.mot_init(ptr, bytecode.length);
    if (rc !== 0) return new Response('Init failed', { status: 500 });

    rc = ex.mot_render();
    if (rc !== 0 && rc !== -2) return new Response('Render failed', { status: 500 });

    var fullHtml = htmlBuf;
    var chunks = [];
    for (var i = 0; i < fullHtml.length; i += chunkSize) {
        chunks.push(fullHtml.slice(i, i + chunkSize));
    }
    if (chunks.length === 0) chunks.push('');

    var chunkDelay = Math.max(20, Math.floor(latency / 3));
    var encoder = new TextEncoder();

    var stream = new ReadableStream({
        start: function (controller) {
            var idx = 0;
            function pushNext() {
                if (idx >= chunks.length) {
                    controller.close();
                    return;
                }
                controller.enqueue(encoder.encode(chunks[idx++]));
                setTimeout(pushNext, chunkDelay);
            }
            pushNext();
        }
    });

    return new Response(stream, {
        headers: { 'Content-Type': 'text/html; charset=utf-8' }
    });
}
