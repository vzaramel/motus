/**
 * Motus Edge Worker
 *
 * Cloudflare Worker that:
 * 1. Receives browser requests
 * 2. Fetches bytecode from origin (streaming)
 * 3. Interprets bytecode using WASM runtime
 * 4. Streams HTML to browser
 * 5. Fetches components/data as needed during interpretation
 */

import wasmModule from './mot-runtime.wasm';
import {
  detectWasmIncompatibilities,
  renderBytecodeWithWasm,
  renderComponentBytecodeWithWasm,
  componentPathToTagName,
} from './wasm-vm.js';
import {
  buildComponentRpcUrl,
  buildDataRpcPayload,
  buildDataRpcUrl,
  normalizeComponentRef,
} from './runtime-contract.js';
import { isDebugEnabled, RenderTrace, renderTraceScriptTag } from './render-trace.js';
import { buildBytecodeDebugSourceMap, extractBytecodeDebugMetadata } from './bytecode-debug.js';
import { renderDevtoolsBootstrapScript } from './devtools-inline.js';
import {
  renderBrowserWasmBootstrapScript,
  shouldEnableBrowserWasm,
} from './browser-wasm-inline.js';
import { buildReactiveWasmPayload } from './reactive-wasm-module.js';
import { SYSTEM_COMPONENTS } from './system-components.generated.js';
import { SYSTEM_COMPONENT_SOURCE_MAPS } from './system-components.maps.generated.js';

const BROWSER_RPC_BASE_PATH = '/_mot';
const componentBytecodeCache = new Map();
const componentBytecodeMetaCache = new Map();
const bundledComponentBytecodeCache = new Map();

function isDevMode(env) {
  const val = String(env.MOT_DEV || '').trim().toLowerCase();
  return val === '1' || val === 'true';
}

function isHotMode(env) {
  const val = String(env.MOT_HOT || '').trim().toLowerCase();
  return val === '1' || val === 'true';
}

function isDsdEnabled(env) {
  // DSD is enabled explicitly, or auto-enabled in hot mode or debug mode
  const val = String(env.MOT_DSD || '').trim().toLowerCase();
  if (val === '1' || val === 'true') return true;
  return isHotMode(env);
}

function renderLiveReloadScript() {
  return `<script data-mot-dev="live-reload">
(function() {
  var es, timer;
  function connect() {
    es = new EventSource('/_mot/dev/events');
    es.addEventListener('change', function() {
      console.log('[mot-dev] Content changed, reloading...');
      window.location.reload();
    });
    es.addEventListener('connected', function() {
      console.log('[mot-dev] Live-reload connected');
    });
    es.onerror = function() {
      es.close();
      clearTimeout(timer);
      timer = setTimeout(connect, 2000);
    };
  }
  connect();
})();
</script>`;
}

function renderHotReloadScript() {
  return `<script type="module" data-mot-dev="hot-reload">
import { render, html } from 'https://esm.sh/lit-html@3';
import { unsafeHTML } from 'https://esm.sh/lit-html@3/directives/unsafe-html.js';

const RPC_BASE = '/_mot';

/**
 * Walk the DOM (including nested shadow roots) and run callback on every
 * element that matches the predicate.  Required because querySelectorAll
 * does not pierce shadow boundaries.
 */
function walkAll(root, predicate, cb) {
  const walker = (node) => {
    if (node.nodeType !== 1) return;
    if (predicate(node)) cb(node);
    if (node.shadowRoot) {
      Array.from(node.shadowRoot.children).forEach(walker);
    }
    Array.from(node.children || []).forEach(walker);
  };
  Array.from(root.children).forEach(walker);
}

/**
 * Find all live instances of a component (across all shadow roots) by
 * its data-mot-path attribute.
 */
function findLiveInstances(path) {
  const matches = [];
  walkAll(document.documentElement,
    (el) => el.getAttribute && el.getAttribute('data-mot-path') === path,
    (el) => matches.push(el));
  return matches;
}

/**
 * Upgrade any DSD <template shadowrootmode> templates that the parser
 * left attached (e.g. when the document is built via DOMParser, which
 * does not auto-attach shadow roots).
 */
function upgradeShadowTemplates(root) {
  const tpls = root.querySelectorAll
    ? Array.from(root.querySelectorAll('template[shadowrootmode]'))
    : [];
  for (const tpl of tpls) {
    const host = tpl.parentElement;
    if (!host || host.shadowRoot) continue;
    const mode = tpl.getAttribute('shadowrootmode') || 'open';
    const shadow = host.attachShadow({ mode });
    shadow.appendChild(tpl.content.cloneNode(true));
    tpl.remove();
  }
}

/**
 * Walk a parsed document tree and find all elements with the given
 * data-mot-path that have a <template shadowrootmode> child (which is
 * how the server emits DSD components).
 */
function findFreshDsdInstances(root, path) {
  const matches = [];
  const all = root.querySelectorAll
    ? root.querySelectorAll('[data-mot-path="' + path + '"]')
    : [];
  for (const el of all) {
    matches.push(el);
  }
  return matches;
}

function extractTemplateInnerHTML(dsdHost) {
  const tpl = dsdHost.querySelector(':scope > template[shadowrootmode]');
  if (tpl) {
    const tmp = document.createElement('div');
    tmp.appendChild(tpl.content.cloneNode(true));
    return tmp.innerHTML;
  }
  // Fallback: element already had its shadow content inlined
  return dsdHost.innerHTML;
}

async function hotSwapComponent(changedPath) {
  const liveEls = findLiveInstances(changedPath);
  if (liveEls.length === 0) {
    console.log('[mot-hot] No live instances of', changedPath, '— skipping');
    return;
  }
  console.log('[mot-hot] Patching', liveEls.length, 'instance(s) of', changedPath);

  const resp = await fetch(window.location.href, {
    headers: { 'Accept': 'text/html' },
    cache: 'no-store',
  });
  if (!resp.ok) {
    console.error('[mot-hot] Fetch failed:', resp.status);
    return;
  }
  const freshHtml = await resp.text();
  const freshDoc = new DOMParser().parseFromString(freshHtml, 'text/html');
  const freshEls = findFreshDsdInstances(freshDoc, changedPath);

  freshEls.forEach((freshEl, idx) => {
    if (idx >= liveEls.length) return;
    const liveEl = liveEls[idx];
    if (!liveEl.shadowRoot) {
      // Late upgrade: attach a shadow root if the browser somehow skipped DSD
      try { liveEl.attachShadow({ mode: 'open' }); } catch (e) {}
    }
    if (liveEl.shadowRoot) {
      const newInner = extractTemplateInnerHTML(freshEl);
      render(html\`\${unsafeHTML(newInner)}\`, liveEl.shadowRoot);
      console.log('[mot-hot] Patched', changedPath, '#' + idx);
    }
  });
}

async function hotSwapAll() {
  // Collect every (path, count) currently live; group by path then patch
  const seen = new Set();
  walkAll(document.documentElement,
    (el) => el.hasAttribute && el.hasAttribute('data-mot-path'),
    (el) => seen.add(el.getAttribute('data-mot-path')));
  for (const path of seen) {
    await hotSwapComponent(path);
  }
}

let es, reconnectTimer;
function connect() {
  es = new EventSource(RPC_BASE + '/dev/events');
  es.addEventListener('change', async (evt) => {
    try {
      const data = JSON.parse(evt.data);
      const path = data.path || '';
      console.log('[mot-hot] Change detected:', path);
      if (path.startsWith('components/')) {
        await hotSwapComponent(path);
      } else {
        await hotSwapAll();
      }
    } catch (err) {
      console.error('[mot-hot] Error:', err);
    }
  });
  es.addEventListener('connected', () => {
    console.log('[mot-hot] Hot-reload connected (lit-html + DSD)');
  });
  es.onerror = () => {
    es.close();
    clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, 2000);
  };
}
connect();
</script>`;
}

// Bytecode flags (must match bytecode.h)
const BYTECODE_FLAG_AUTH_REQUIRED = 0x0001;
const BYTECODE_FLAG_ADMIN_REQUIRED = 0x0002;
const VM_STEP_DEBUG_MAX_STEPS = 1800;
const VM_STEP_DEBUG_MAX_SOURCES = 24;
const VM_STEP_DEBUG_MAX_SOURCE_BYTES = 120000;
const debugSourceCache = new Map();
const OPCODE_NAMES = [
  'BC_NOP',
  'BC_CONST',
  'BC_POP',
  'BC_DUP',
  'BC_LOAD',
  'BC_STORE',
  'BC_LOAD_GLOBAL',
  'BC_LOAD_FIELD',
  'BC_LOAD_INDEX',
  'BC_STORE_FIELD',
  'BC_STORE_INDEX',
  'BC_NULL',
  'BC_TRUE',
  'BC_FALSE',
  'BC_INT',
  'BC_ADD',
  'BC_SUB',
  'BC_MUL',
  'BC_DIV',
  'BC_MOD',
  'BC_NEG',
  'BC_EQ',
  'BC_NEQ',
  'BC_LT',
  'BC_LTE',
  'BC_GT',
  'BC_GTE',
  'BC_AND',
  'BC_OR',
  'BC_NOT',
  'BC_JUMP',
  'BC_JUMP_IF_FALSE',
  'BC_JUMP_IF_TRUE',
  'BC_ITER_START',
  'BC_ITER_NEXT',
  'BC_ITER_END',
  'BC_EMIT_LITERAL',
  'BC_EMIT_TEXT',
  'BC_EMIT_RAW',
  'BC_EMIT_ATTR_START',
  'BC_EMIT_ATTR_END',
  'BC_EMIT_TAG_OPEN',
  'BC_EMIT_TAG_END',
  'BC_EMIT_TAG_CLOSE',
  'BC_EMIT_TAG_SELF',
  'BC_FETCH_DATA',
  'BC_FETCH_WAIT',
  'BC_CALL',
  'BC_CALL_BUILTIN',
  'BC_CALL_PIPE',
  'BC_RETURN',
  'BC_COMPONENT_START',
  'BC_COMPONENT_END',
  'BC_COMPONENT_LOAD',
  'BC_SLOT_START',
  'BC_SLOT_END',
  'BC_SLOT_DEFAULT',
  'BC_DEP_START',
  'BC_DEP_END',
  'BC_ARRAY_NEW',
  'BC_OBJECT_NEW',
  'BC_OBJECT_SET',
  'BC_CONCAT',
  'BC_HALT',
  'BC_COMPONENT_LINKED',
];

function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

function decodeBase64ToBytes(base64) {
  if (!base64 || typeof base64 !== 'string') return null;
  try {
    const normalized = base64.trim();
    const raw = atob(normalized);
    const out = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) {
      out[i] = raw.charCodeAt(i) & 0xFF;
    }
    return out;
  } catch {
    return null;
  }
}

function encodeBytesToBase64(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength === 0) return '';
  let binary = '';
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    binary += String.fromCharCode(...chunk);
  }
  return btoa(binary);
}

function getBundledComponentBytecode(componentPath) {
  const key = String(componentPath || '');
  if (!key) return null;
  if (bundledComponentBytecodeCache.has(key)) {
    return bundledComponentBytecodeCache.get(key);
  }
  const base64 = SYSTEM_COMPONENTS?.[key];
  if (!base64) {
    bundledComponentBytecodeCache.set(key, null);
    return null;
  }
  const bytes = decodeBase64ToBytes(base64);
  bundledComponentBytecodeCache.set(key, bytes || null);
  return bytes || null;
}

/**
 * Parse cookies from request
 */
function parseCookies(request) {
  const cookies = {};
  const cookieHeader = request.headers.get('Cookie');
  if (cookieHeader) {
    cookieHeader.split(';').forEach(cookie => {
      const [name, ...rest] = cookie.trim().split('=');
      if (name) {
        cookies[name] = rest.join('=');
      }
    });
  }
  return cookies;
}

/**
 * Validate session with origin server
 */
async function validateSession(originUrl, sessionToken) {
  if (!sessionToken) {
    return { valid: false };
  }

  try {
    const response = await fetch(`${originUrl}/session`, {
      headers: {
        'Cookie': `session=${sessionToken}`,
      },
    });

    if (!response.ok) {
      return { valid: false };
    }

    return await response.json();
  } catch (e) {
    console.error('[edge] Session validation error:', e);
    return { valid: false };
  }
}

/**
 * Get bytecode flags from header (bytes 8-9, little-endian)
 */
function getBytecodeFlags(bytecode) {
  if (bytecode.length < 10) return 0;
  const view = new DataView(bytecode.buffer, bytecode.byteOffset, bytecode.byteLength);
  return view.getUint16(8, true); // little-endian
}

function resolveVmMode(mode) {
  const normalized = String(mode || 'wasm').toLowerCase();
  if (normalized !== 'wasm') {
    console.warn(`[edge] Unsupported MOT_VM="${normalized}". Forcing wasm mode.`);
  }
  return 'wasm';
}

function shouldTrackDependency(path) {
  return typeof path === 'string' && path.length > 0 && !path.startsWith('@');
}

async function fetchPageBytecode(originUrl, pageName) {
  const pageResponse = await fetch(`${originUrl}/page/${pageName}`);
  if (!pageResponse.ok) {
    return {
      ok: false,
      status: pageResponse.status,
      bytecode: null,
    };
  }

  const reader = pageResponse.body.getReader();
  const chunks = [];
  let totalBytes = 0;

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    totalBytes += value.length;
    console.log(`[edge] Received ${value.length} bytes (total: ${totalBytes})`);
  }

  const bytecode = new Uint8Array(totalBytes);
  let offset = 0;
  for (const chunk of chunks) {
    bytecode.set(chunk, offset);
    offset += chunk.length;
  }

  return { ok: true, status: 200, bytecode, totalBytes };
}

async function renderPlaygroundSourceWithWasm({ request, originUrl, debugEnabled = false }) {
  const compileResponse = await fetch(`${originUrl}/playground/compile`, {
    method: 'POST',
    headers: {
      'Content-Type': request.headers.get('Content-Type') || 'application/json',
      'Cookie': request.headers.get('Cookie') || '',
    },
    body: await request.text(),
  });

  if (!compileResponse.ok) {
    return new Response(await compileResponse.text(), {
      status: compileResponse.status,
      headers: {
        'Content-Type': compileResponse.headers.get('Content-Type') || 'application/json',
        'Cache-Control': 'no-store',
      },
    });
  }

  const bytecode = new Uint8Array(await compileResponse.arrayBuffer());
  const compatibility = detectWasmIncompatibilities(bytecode);
  if (compatibility.unsupported) {
    return new Response(JSON.stringify({
      error: `Unsupported bytecode for wasm VM: ${compatibility.reasons.join(', ')}`,
    }), {
      status: 400,
      headers: {
        'Content-Type': 'application/json',
        'Cache-Control': 'no-store',
      },
    });
  }

  let html = '';
  const result = await renderBytecodeWithWasm({
    wasmModule,
    bytecode,
    output: async (text) => {
      html += text;
    },
    fetchData: async () => {
      throw new Error('Playground data queries are not supported in this endpoint');
    },
    loadComponent: async (ref) => {
      const name = ref?.path || ref?.name || 'unknown';
      throw new Error(`Playground dynamic component loading is not supported: ${name}`);
    },
    enableVmSourceStepping: debugEnabled,
  });

  if (!result.success) {
    return new Response(JSON.stringify({ error: result.error || 'WASM render failed' }), {
      status: 400,
      headers: {
        'Content-Type': 'application/json',
        'Cache-Control': 'no-store',
      },
    });
  }

  return new Response(JSON.stringify({ html }), {
    status: 200,
    headers: {
      'Content-Type': 'application/json',
      'Cache-Control': 'no-store',
    },
  });
}

function fetchModeFromCallKind(callKind) {
  const kind = String(callKind || 'page');
  if (kind === 'dynamic_vm' || kind === 'linked_vm' || kind === 'linked_edge') {
    return 'edge_delayed';
  }
  return 'origin_first_render';
}

async function fetchDataFromOrigin(originUrl, pageName, req, params, trace, context = null, onDataFetch = null) {
  const startedAt = nowMs();
  console.log(`[edge] Fetching data ref=${req.queryRef} page=${pageName}`);
  const queryRef = Number(req?.queryRef ?? 0) >>> 0;
  const signature = Number(req?.signature ?? 0) >>> 0;
  const name = req?.name ? String(req.name) : '';
  const componentPath = context?.componentPath ? String(context.componentPath) : '';
  const componentCallKind = context?.componentCallKind ? String(context.componentCallKind) : 'page';
  const fetchMode = context?.fetchMode
    ? String(context.fetchMode)
    : fetchModeFromCallKind(componentCallKind);
  const response = await fetch(buildDataRpcUrl(originUrl, pageName), {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(buildDataRpcPayload(req, params)),
  });
  if (!response.ok) {
    const detail = await response.text().catch(() => '');
    const endedAt = nowMs();
    const durationMs = endedAt - startedAt;
    const event = {
      page: pageName,
      queryRef,
      signature,
      name,
      fetchMode,
      componentPath,
      componentCallKind,
      status: `http_${response.status}`,
      durationMs,
      rowCount: 0,
      error: detail,
      startedAtMs: startedAt,
      responseReceivedAtMs: endedAt,
      endedAtMs: endedAt,
      parseMs: 0,
    };
    if (trace) {
      trace.recordDataFetch(event);
    }
    if (typeof onDataFetch === 'function') {
      onDataFetch(event);
    }
    throw new Error(
      `Failed to fetch data for queryRef=${req.queryRef} status=${response.status} body=${detail}`,
    );
  }
  const responseReceivedAt = nowMs();
  const json = await response.json();
  const endedAt = nowMs();
  const durationMs = endedAt - startedAt;
  const parseMs = Math.max(0, endedAt - responseReceivedAt);
  const rowCount = Array.isArray(json) ? json.length : (json == null ? 0 : 1);
  const event = {
    page: pageName,
    queryRef,
    signature,
    name,
    fetchMode,
    componentPath,
    componentCallKind,
    status: 'ok',
    rowCount,
    durationMs,
    startedAtMs: startedAt,
    responseReceivedAtMs: responseReceivedAt,
    endedAtMs: endedAt,
    parseMs,
  };
  if (trace) {
    trace.recordDataFetch(event);
  }
  if (typeof onDataFetch === 'function') {
    onDataFetch(event);
  }
  if (req.isSingle && Array.isArray(json)) {
    return json[0] || null;
  }
  return json;
}

function deriveComponentPathFromSource(sourcePath) {
  const src = String(sourcePath || '').trim();
  if (!src) return '';
  if (src.startsWith('components/') || src.startsWith('pages/')) {
    if (src.endsWith('.mot')) {
      return src.slice(0, -4);
    }
    return src;
  }
  return '';
}

function normalizeSourcePath(sourcePath) {
  let src = String(sourcePath || '').trim();
  while (src.startsWith('./')) {
    src = src.slice(2);
  }
  if (src.startsWith('/')) {
    src = src.slice(1);
  }
  if (!src) return '';
  if ((src.startsWith('components/') || src.startsWith('pages/')) && !src.endsWith('.mot')) {
    return `${src}.mot`;
  }
  return src;
}

function opcodeName(opcode) {
  const idx = Number(opcode ?? 0) >>> 0;
  return OPCODE_NAMES[idx] || `OP_${idx}`;
}

function isSafeDebugSourcePath(sourcePath) {
  const path = String(sourcePath || '').trim();
  if (!path) return false;
  if (!(path.startsWith('components/') || path.startsWith('pages/'))) return false;
  if (!path.endsWith('.mot')) return false;
  if (path.includes('..')) return false;
  return /^[A-Za-z0-9_./-]+$/.test(path);
}

async function fetchDebugSourceTextFromOrigin(originUrl, sourcePath) {
  const normalized = normalizeSourcePath(sourcePath);
  if (!isSafeDebugSourcePath(normalized)) return null;
  const cacheKey = `${originUrl}|${normalized}`;
  if (debugSourceCache.has(cacheKey)) {
    return debugSourceCache.get(cacheKey);
  }
  const pending = (async () => {
    const response = await fetch(`${originUrl}/source/${normalized}`);
    if (!response.ok) return null;
    const text = await response.text();
    return text.slice(0, VM_STEP_DEBUG_MAX_SOURCE_BYTES);
  })()
    .catch(() => null);
  debugSourceCache.set(cacheKey, pending);
  return pending;
}

function createVmStepDebugState(pageName) {
  return {
    mode: 'step',
    enabled: true,
    pageSourcePath: normalizeSourcePath(`pages/${pageName}.mot`),
    startedAt: nowMs(),
    totalSteps: 0,
    truncated: false,
    steps: [],
    sourcePaths: new Set(),
  };
}

function recordVmStepDebugEvent(state, event) {
  if (!state || !event) return;
  state.totalSteps += 1;

  const scope = event.scope && typeof event.scope === 'object' ? event.scope : {};
  const eventSourcePath = normalizeSourcePath(event.sourcePath || '');
  const scopeSourcePath = normalizeSourcePath(scope.sourcePath || '');
  const scopeComponentPath = scope.componentPath ? String(scope.componentPath) : '';
  let sourcePath = eventSourcePath || scopeSourcePath || state.pageSourcePath;
  if (
    scopeSourcePath &&
    scopeComponentPath.startsWith('components/') &&
    sourcePath === state.pageSourcePath
  ) {
    sourcePath = scopeSourcePath;
  }
  if (sourcePath && isSafeDebugSourcePath(sourcePath) && state.sourcePaths.size < VM_STEP_DEBUG_MAX_SOURCES) {
    state.sourcePaths.add(sourcePath);
  }

  if (state.steps.length >= VM_STEP_DEBUG_MAX_STEPS) {
    state.truncated = true;
    return;
  }

  state.steps.push({
    index: state.steps.length,
    atMs: Math.max(0, nowMs() - state.startedAt),
    chunkKind: Number(event.chunkKind ?? 0) >>> 0,
    chunkIndex: Number(event.chunkIndex ?? 0) >>> 0,
    pc: Number(event.pc ?? 0) >>> 0,
    opcode: Number(event.opcode ?? 0) >>> 0,
    opcodeName: opcodeName(event.opcode),
    line: Number(event.line ?? 0) >>> 0,
    column: Number(event.column ?? 0) >>> 0,
    sourcePath: sourcePath || '',
    scope: {
      componentPath: scope.componentPath ? String(scope.componentPath) : `pages/${state.pageSourcePath.slice(6, -4)}`,
      sourcePath: scopeSourcePath || sourcePath || state.pageSourcePath,
      componentOpcode: scope.componentOpcode ? String(scope.componentOpcode) : 'n/a',
      callKind: scope.callKind ? String(scope.callKind) : 'page',
      loadSource: scope.loadSource ? String(scope.loadSource) : 'page',
      renderEngine: scope.renderEngine ? String(scope.renderEngine) : 'wasm-vm',
      scopeStack: Array.isArray(scope.scopeStack) ? scope.scopeStack.slice(0, 16).map((entry) => String(entry)) : [],
    },
  });
}

async function finalizeVmStepDebug(state, originUrl) {
  if (!state) return null;
  const sources = {};
  for (const sourcePath of state.sourcePaths) {
    const text = await fetchDebugSourceTextFromOrigin(originUrl, sourcePath);
    if (typeof text === 'string') {
      sources[sourcePath] = text;
    }
  }
  return {
    mode: state.mode,
    enabled: state.enabled,
    truncated: state.truncated,
    totalSteps: state.totalSteps,
    maxSteps: VM_STEP_DEBUG_MAX_STEPS,
    steps: state.steps,
    sources,
  };
}

function buildWasmComponentDebugMetaByFunction(debugMeta, fallback = {}) {
  const out = Object.create(null);
  const sizeByFunction = Object.create(null);
  const linkedByPath = new Set();
  const sizes = Array.isArray(debugMeta?.functionChunkSizes) ? debugMeta.functionChunkSizes : [];
  const linkedRefs = Array.isArray(debugMeta?.linkedComponentRefs) ? debugMeta.linkedComponentRefs : [];
  for (const entry of linkedRefs) {
    if (entry?.path) linkedByPath.add(String(entry.path));
  }
  for (const entry of sizes) {
    const chunkKind = Number(entry?.chunkKind ?? 1) >>> 0;
    if (chunkKind !== 1) continue;
    const chunkIndex = Number(entry?.chunkIndex ?? 0) >>> 0;
    sizeByFunction[String(chunkIndex)] = Number(entry?.sizeBytes ?? 0) >>> 0;
  }

  // First, register all LINKED component refs by their resolved func_idx.
  // The linker writes "@linked_ref:REF:FUNC" deps so the runtime can map
  // ref_idx → func_idx; debug-extract turns that into linkedComponentRefs.
  // These functions are NOT in functionComponents (they live in linked
  // bundles), so without this we lose the path → tag mapping for DSD.
  for (const entry of linkedRefs) {
    if (!entry?.path) continue;
    const funcIndex = Number(entry.funcIndex ?? -1);
    if (!Number.isFinite(funcIndex) || funcIndex < 0) continue;
    const componentPath = String(entry.path);
    const sourcePath = `${componentPath}.mot`;
    const componentName = entry.name
      ? String(entry.name)
      : componentPath.replace(/^components\//, '');
    out[String(funcIndex)] = {
      componentName,
      componentPath,
      sourcePath,
      renderEngine: 'wasm-vm',
      loadSource: 'linked_internal',
      artifactSource: 'edge-linked-bytecode',
      callKind: 'linked_internal',
      componentOpcode: 'BC_COMPONENT_LINKED',
      sizeBytes: Number(sizeByFunction[String(funcIndex)] ?? 0),
    };
  }

  // Then layer inline component metadata on top (page-local components).
  const list = Array.isArray(debugMeta?.functionComponents) ? debugMeta.functionComponents : [];
  for (const raw of list) {
    const chunkKind = Number(raw?.chunkKind ?? 1) >>> 0;
    const chunkIndex = Number(raw?.chunkIndex ?? 0) >>> 0;
    if (chunkKind !== 1) continue;
    const sourcePath = normalizeSourcePath(raw?.sourcePath ? String(raw.sourcePath) : (fallback.sourcePath || ''));
    const componentPath = deriveComponentPathFromSource(sourcePath) || fallback.componentPath || '';
    const componentName = raw?.name
      ? String(raw.name)
      : (fallback.componentName || (componentPath ? componentPath.replace(/^components\//, '') : `component#${chunkIndex}`));
    const isLinkedLowered = componentPath && linkedByPath.has(componentPath);
    out[String(chunkIndex)] = {
      componentName,
      componentPath,
      sourcePath,
      renderEngine: 'wasm-vm',
      loadSource: isLinkedLowered ? 'linked_internal' : (fallback.loadSource || 'static'),
      artifactSource: isLinkedLowered ? 'edge-linked-bytecode' : (fallback.artifactSource || 'inline'),
      callKind: isLinkedLowered ? 'linked_internal' : 'static_origin',
      componentOpcode: isLinkedLowered ? 'BC_COMPONENT_LINKED' : 'BC_COMPONENT_START',
      sizeBytes: Number(sizeByFunction[String(chunkIndex)] ?? 0),
    };
  }
  return Object.keys(out).length > 0 ? out : null;
}

async function loadComponentBytecode(originUrl, ref, trace) {
  const startedAt = performance.now();
  const normalizedRef = normalizeComponentRef(ref);
  const componentOpcode = ref?.componentOpcode === 'BC_COMPONENT_LINKED'
    ? 'BC_COMPONENT_LINKED'
    : 'BC_COMPONENT_LOAD';

  if (componentOpcode === 'BC_COMPONENT_LINKED') {
    const bundled = getBundledComponentBytecode(normalizedRef.path);
    if (bundled instanceof Uint8Array) {
      const bundledArtifactSource = 'edge-bundled-bytecode';
      componentBytecodeCache.set(normalizedRef.path, bundled);
      componentBytecodeMetaCache.set(normalizedRef.path, {
        loadSource: 'linked_bundle',
        artifactSource: bundledArtifactSource,
      });
      if (trace) {
        trace.recordComponentLoad({
          path: normalizedRef.path,
          source: 'linked_bundle',
          artifactSource: bundledArtifactSource,
          status: 'ok',
          sizeBytes: bundled.byteLength,
          durationMs: performance.now() - startedAt,
        });
      }
      return {
        bytecode: bundled,
        path: normalizedRef.path,
        name: normalizedRef.name,
        loadSource: 'linked_bundle',
        artifactSource: bundledArtifactSource,
      };
    }
    if (trace) {
      trace.recordComponentLoad({
        path: normalizedRef.path,
        source: 'linked_bundle',
        artifactSource: 'edge-bundled-bytecode',
        status: 'missing',
        durationMs: performance.now() - startedAt,
        error: `Linked component not embedded in edge bundle: ${normalizedRef.path}`,
      });
    }
    throw new Error(`Linked component not embedded in edge bundle: ${normalizedRef.path}`);
  }

  const cached = componentBytecodeCache.get(normalizedRef.path);
  if (cached instanceof Uint8Array) {
    const cachedMeta = componentBytecodeMetaCache.get(normalizedRef.path) || {
      loadSource: 'cache',
      artifactSource: 'origin',
    };
    if (trace) {
      trace.recordComponentLoad({
        path: normalizedRef.path,
        source: 'cache',
        artifactSource: cachedMeta.artifactSource || 'origin',
        status: 'ok',
        durationMs: performance.now() - startedAt,
      });
    }
    return {
      bytecode: cached,
      path: normalizedRef.path,
      name: normalizedRef.name,
      loadSource: cachedMeta.loadSource || 'cache',
      artifactSource: cachedMeta.artifactSource || 'origin',
    };
  }

  const response = await fetch(buildComponentRpcUrl(originUrl, normalizedRef.path));
  if (!response.ok) {
    if (trace) {
      trace.recordComponentLoad({
        path: normalizedRef.path,
        source: 'origin',
        artifactSource: 'origin',
        status: `http_${response.status}`,
        durationMs: performance.now() - startedAt,
      });
    }
    throw new Error(`Failed to load component bytecode: ${normalizedRef.path} (${response.status})`);
  }

  const bytecode = new Uint8Array(await response.arrayBuffer());
  componentBytecodeCache.set(normalizedRef.path, bytecode);
  componentBytecodeMetaCache.set(normalizedRef.path, {
    loadSource: 'origin',
    artifactSource: 'origin',
  });
  if (trace) {
    trace.recordComponentLoad({
      path: normalizedRef.path,
      source: 'origin',
      artifactSource: 'origin',
      status: 'ok',
      sizeBytes: bytecode.byteLength,
      durationMs: performance.now() - startedAt,
    });
  }
  return {
    bytecode,
    path: normalizedRef.path,
    name: normalizedRef.name,
    loadSource: 'origin',
    artifactSource: 'origin',
  };
}

async function renderDynamicComponent(
  originUrl,
  pageName,
  ref,
  props,
  children,
  dependencies,
  trace,
  options = {},
) {
  const {
    debugEnabled = false,
    onComponentRender = null,
    onDebugStep = null,
    onDataFetch = null,
    onVmAwait = null,
    onVmResume = null,
    enableDSD = false,
  } = options;
  const emitComponentEvent = (event) => {
    recordComponentRenderTrace(trace, event);
    if (typeof onComponentRender === 'function') {
      onComponentRender(event);
    }
  };
  const emitDebugStep = (event) => {
    if (typeof onDebugStep === 'function') {
      onDebugStep(event);
    }
  };
  const renderStartedAt = performance.now();
  const normalizedRef = normalizeComponentRef(ref);
  const componentPath = normalizedRef.path;
  const componentOpcode = ref?.componentOpcode === 'BC_COMPONENT_LINKED'
    ? 'BC_COMPONENT_LINKED'
    : 'BC_COMPONENT_LOAD';
  const callKind = componentOpcode === 'BC_COMPONENT_LINKED' ? 'linked_vm' : 'dynamic_vm';
  let loadSource = 'origin';
  let artifactSource = 'origin';
  let componentSizeBytes = 0;

  try {
    const componentPayload = await loadComponentBytecode(
      originUrl,
      { ...normalizedRef, componentOpcode },
      trace,
    );
    loadSource = componentPayload.loadSource || 'origin';
    artifactSource = componentPayload.artifactSource || 'origin';
    componentSizeBytes = componentPayload.bytecode?.byteLength ?? 0;
    if (trace && loadSource === 'linked_bundle') {
      const bundledMap = SYSTEM_COMPONENT_SOURCE_MAPS?.[componentPath];
      if (bundledMap && typeof bundledMap === 'object') {
        trace.recordComponentSourceMap({
          path: componentPath,
          source: 'linked_bundle',
          sourceMap: bundledMap,
        });
      }
    }

    emitComponentEvent({
      phase: 'start',
      status: 'ok',
      path: componentPath,
      renderEngine: 'wasm-vm',
      callKind,
      componentOpcode,
      loadSource,
      artifactSource,
      sizeBytes: componentSizeBytes,
    });

    const componentDebugMeta = debugEnabled
      ? extractBytecodeDebugMetadata(componentPayload.bytecode)
      : null;
    const componentDebugMetaByFunction = buildWasmComponentDebugMetaByFunction(
      componentDebugMeta,
      {
        componentPath,
        componentName: normalizedRef.name,
        sourcePath: `${componentPath}.mot`,
        loadSource,
        artifactSource,
      },
    );
    const wasmResult = await renderComponentBytecodeWithWasm({
      wasmModule,
      bytecode: componentPayload.bytecode,
      functionIndex: 0,
      props,
      children,
      fetchData: async (req, params, context) => fetchDataFromOrigin(
        originUrl,
        pageName,
        req,
        params,
        trace,
        {
          ...context,
          componentPath: context?.componentPath || componentPath,
          componentCallKind: context?.componentCallKind || callKind,
        },
        onDataFetch,
      ),
      onDependency: (event, path) => {
        if (event === 'start' && shouldTrackDependency(path)) {
          dependencies.push(path);
        }
      },
      loadComponent: async (nestedRef) => renderDynamicComponent(
        originUrl,
        pageName,
        {
          name: nestedRef.name,
          path: nestedRef.path,
          componentOpcode: nestedRef.componentOpcode,
        },
        nestedRef.props,
        nestedRef.children,
        dependencies,
        trace,
        {
          debugEnabled,
          onComponentRender: emitComponentEvent,
          onDebugStep: emitDebugStep,
          onDataFetch,
          onVmAwait,
          onVmResume,
          enableDSD,
        },
      ),
      onComponentRender: emitComponentEvent,
      componentDebugMetaByFunction,
      enableComponentDebugWrap: debugEnabled,
      enableVmSourceStepping: debugEnabled,
      onDebugStep: emitDebugStep,
      fetchScope: { componentPath, componentCallKind: callKind },
      onVmAwait,
      onVmResume,
      enableDSD,
    });
    if (!wasmResult.success) {
      throw new Error(wasmResult.error || `WASM component execution failed: ${componentPath}`);
    }
    emitComponentEvent({
      phase: 'end',
      path: componentPath,
      status: 'ok',
      durationMs: performance.now() - renderStartedAt,
      renderEngine: 'wasm-vm',
      callKind,
      componentOpcode,
      loadSource,
      artifactSource,
      sizeBytes: componentSizeBytes,
    });
    let html = wasmResult.html || '';
    if (enableDSD && componentPath) {
      const tag = componentPathToTagName(componentPath);
      html = `<${tag} data-mot-path="${componentPath}"><template shadowrootmode="open">${html}</template></${tag}>`;
    }
    return html;
  } catch (error) {
    emitComponentEvent({
      phase: 'end',
      path: componentPath,
      status: 'error',
      durationMs: performance.now() - renderStartedAt,
      renderEngine: 'wasm-vm',
      callKind,
      componentOpcode,
      loadSource,
      artifactSource,
      sizeBytes: componentSizeBytes,
      error: error?.message || String(error),
    });
    throw error;
  }
}

function recordComponentRenderTrace(trace, event) {
  if (!trace || !event) return;
  const phase = event.phase ? String(event.phase) : 'end';
  if (phase !== 'end') return;
  const renderEngine = event.renderEngine ? String(event.renderEngine) : 'unknown';
  trace.recordComponentRender({
    path: event.path || '',
    phase,
    status: event.status || 'ok',
    durationMs: Number(event.durationMs ?? 0),
    renderEngine,
    callKind: event.callKind || 'unknown',
    componentOpcode: event.componentOpcode || null,
    loadSource: event.loadSource || 'unknown',
    artifactSource: event.artifactSource || null,
    sizeBytes: Number(event.sizeBytes ?? 0),
    error: event.error || null,
  });
  if (renderEngine === 'wasm-native') {
    trace.recordExecutionPath('component:wasm-native');
  } else if (renderEngine === 'wasm-vm') {
    trace.recordExecutionPath('component:wasm-vm');
  }
}

async function runWithWasmInterpreter(options) {
  const {
    bytecode, writer, encoder, originUrl, pageName, dependencies, trace, debugEnabled,
    componentDebugMetaByFunction = null,
    onComponentRender = null,
    onDebugStep = null,
    onDataFetch = null,
    onVmAwait = null,
    onVmResume = null,
    enableDSD = false,
  } = options;
  const componentStack = [];
  const emitComponentEvent = (event) => {
    const phase = event?.phase ? String(event.phase) : 'end';
    const path = event?.path ? String(event.path) : '';
    if (phase === 'start' && path) {
      componentStack.push({
        path,
        componentOpcode: event.componentOpcode || 'BC_COMPONENT_START',
        callKind: event.callKind || 'static_origin',
        loadSource: event.loadSource || 'static',
        renderEngine: event.renderEngine || 'wasm-vm',
      });
    } else if (phase === 'end' && path) {
      for (let i = componentStack.length - 1; i >= 0; i--) {
        if (componentStack[i].path === path) {
          componentStack.splice(i, 1);
          break;
        }
      }
    }
    recordComponentRenderTrace(trace, event);
    if (typeof onComponentRender === 'function') {
      onComponentRender(event);
    }
  };
  let sourceStepObserved = false;
  const emitDebugStep = (event) => {
    if (trace && !sourceStepObserved) {
      sourceStepObserved = true;
      trace.recordExecutionPath('vm:source-step');
    }
    if (typeof onDebugStep === 'function') {
      const active = componentStack.length > 0 ? componentStack[componentStack.length - 1] : null;
      onDebugStep({
        ...event,
        scope: {
          componentPath: active?.path || `pages/${pageName}`,
          sourcePath: active?.path ? `${active.path}.mot` : `pages/${pageName}.mot`,
          componentOpcode: active?.componentOpcode || 'n/a',
          callKind: active?.callKind || 'page',
          loadSource: active?.loadSource || 'page',
          renderEngine: active?.renderEngine || 'wasm-vm',
          scopeStack: componentStack.map((entry) => entry.path),
        },
      });
    }
  };
  if (trace) trace.recordExecutionPath('vm:wasm');

  const compatibility = detectWasmIncompatibilities(bytecode);
  if (compatibility.unsupported) {
    return {
      success: false,
      fallbackReason: `Unsupported bytecode for wasm VM: ${compatibility.reasons.join(', ')}`,
    };
  }

  const result = await renderBytecodeWithWasm({
    wasmModule,
    bytecode,
    output: async (text) => {
      await writer.write(encoder.encode(text));
    },
    fetchData: async (req, params, context) => fetchDataFromOrigin(
      originUrl,
      pageName,
      req,
      params,
      trace,
      context,
      onDataFetch,
    ),
    onDependency: (event, path) => {
      if (event === 'start' && shouldTrackDependency(path)) {
        dependencies.push(path);
      }
    },
    loadComponent: async (ref) => renderDynamicComponent(
      originUrl,
      pageName,
      {
        name: ref.name,
        path: ref.path,
        componentOpcode: ref.componentOpcode,
      },
      ref.props,
      ref.children,
      dependencies,
      trace,
      {
        debugEnabled,
        onComponentRender: emitComponentEvent,
        onDebugStep: emitDebugStep,
        onDataFetch,
        onVmAwait,
        onVmResume,
        enableDSD,
      },
    ),
    onComponentRender: emitComponentEvent,
    componentDebugMetaByFunction,
    enableComponentDebugWrap: debugEnabled,
    enableVmSourceStepping: debugEnabled,
    onDebugStep: emitDebugStep,
    fetchScope: { componentPath: `pages/${pageName}`, componentCallKind: 'page' },
    onVmAwait,
    onVmResume,
    enableDSD,
  });

  if (!result.success) {
    return {
      success: false,
      fallbackReason: result.error || 'WASM interpreter failed',
    };
  }

  return { success: true, vmUsed: 'wasm' };
}

/**
 * Stream response using TransformStream
 */
function createStreamingResponse(readable) {
  return new Response(readable, {
    headers: {
      'Content-Type': 'text/html; charset=utf-8',
      'Transfer-Encoding': 'chunked',
      'X-Powered-By': 'Motus Edge Runtime',
    },
  });
}

function formatBytes(value) {
  const bytes = Number(value ?? 0);
  if (!Number.isFinite(bytes) || bytes <= 0) return 'n/a';
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function formatMs(value) {
  const ms = Number(value ?? 0);
  if (!Number.isFinite(ms) || ms < 0) return '0.0';
  return ms.toFixed(1);
}

class LinkingGraphTracker {
  constructor({ pageName, pageSizeBytes }) {
    this.rootId = `pages/${pageName}`;
    this.nodes = new Map();
    this.edges = new Map();
    this.stack = [];
    this.ensureNode({
      path: this.rootId,
      callKind: 'page',
      renderEngine: 'wasm-vm',
      loadSource: 'page',
      artifactSource: 'origin',
      sizeBytes: pageSizeBytes,
      nodeType: 'component',
    });
  }

  ensureNode(event) {
    const path = event?.path ? String(event.path) : '(unknown)';
    const nodeType = event?.nodeType === 'data_fetch' ? 'data_fetch' : 'component';
    let node = this.nodes.get(path);
    if (!node) {
      node = {
        id: path,
        nodeType,
        path,
        label: event?.label ? String(event.label) : path,
        callKinds: new Set(),
        componentOpcodes: new Set(),
        renderEngines: new Set(),
        loadSources: new Set(),
        artifactSources: new Set(),
        fetchModes: new Set(),
        queryRefs: new Set(),
        signatures: new Set(),
        queryNames: new Set(),
        sizeBytes: 0,
        renderCount: 0,
        fetchCount: 0,
        rowCount: 0,
        errorCount: 0,
        totalMs: 0,
        selfMs: 0,
      };
      this.nodes.set(path, node);
    }
    if (node.nodeType !== nodeType) {
      node.nodeType = nodeType;
    }
    if (event?.label) node.label = String(event.label);
    if (event?.callKind) node.callKinds.add(String(event.callKind));
    if (event?.componentOpcode) node.componentOpcodes.add(String(event.componentOpcode));
    if (event?.renderEngine) node.renderEngines.add(String(event.renderEngine));
    if (event?.loadSource) node.loadSources.add(String(event.loadSource));
    if (event?.artifactSource) node.artifactSources.add(String(event.artifactSource));
    if (event?.fetchMode) node.fetchModes.add(String(event.fetchMode));
    if (event?.queryRef != null) node.queryRefs.add(Number(event.queryRef) >>> 0);
    if (event?.signature != null) node.signatures.add(Number(event.signature) >>> 0);
    if (event?.name) node.queryNames.add(String(event.name));
    const sizeBytes = Number(event?.sizeBytes ?? 0);
    if (Number.isFinite(sizeBytes) && sizeBytes > 0) {
      node.sizeBytes = Math.max(node.sizeBytes, sizeBytes);
    }
    return node;
  }

  ensureEdge({ from, to, edgeType = 'component', callKind = 'unknown' }) {
    const key = `${from}=>${to}=>${edgeType}=>${callKind}`;
    let edge = this.edges.get(key);
    if (!edge) {
      edge = {
        key,
        from,
        to,
        edgeType,
        callKind,
        componentOpcodes: new Set(),
        fetchModes: new Set(),
        queryRefs: new Set(),
        rowCount: 0,
        count: 0,
        totalMs: 0,
        errorCount: 0,
      };
      this.edges.set(key, edge);
    }
    return edge;
  }

  beginRoot() {
    if (this.stack.length > 0) return;
    this.stack.push({
      nodeId: this.rootId,
      startedAt: nowMs(),
      childMs: 0,
      edgeKey: null,
    });
  }

  endRoot(durationMs, status = 'ok') {
    this.endEvent({
      phase: 'end',
      path: this.rootId,
      callKind: 'page',
      renderEngine: 'wasm-vm',
      loadSource: 'page',
      artifactSource: 'origin',
      durationMs,
      status,
    });
  }

  startEvent(event) {
    const node = this.ensureNode(event);
    const parentFrame = this.stack.length > 0 ? this.stack[this.stack.length - 1] : null;
    let edgeKey = null;
    if (parentFrame) {
      const callKind = event?.callKind ? String(event.callKind) : 'unknown';
      const componentOpcode = event?.componentOpcode ? String(event.componentOpcode) : 'unknown';
      edgeKey = `${parentFrame.nodeId}=>${node.id}=>component=>${callKind}`;
      const edge = this.ensureEdge({
        from: parentFrame.nodeId,
        to: node.id,
        edgeType: 'component',
        callKind,
      });
      edge.componentOpcodes.add(componentOpcode);
      edge.count += 1;
    }
    this.stack.push({
      nodeId: node.id,
      startedAt: nowMs(),
      childMs: 0,
      edgeKey,
    });
  }

  endEvent(event) {
    const node = this.ensureNode(event);
    let frameIndex = -1;
    for (let i = this.stack.length - 1; i >= 0; i--) {
      if (this.stack[i].nodeId === node.id) {
        frameIndex = i;
        break;
      }
    }
    if (frameIndex < 0) return;
    while (this.stack.length - 1 > frameIndex) {
      this.stack.pop();
    }

    const frame = this.stack.pop();
    const explicitDuration = Number(event?.durationMs);
    const totalMs = Number.isFinite(explicitDuration) && explicitDuration >= 0
      ? explicitDuration
      : Math.max(0, nowMs() - frame.startedAt);
    const selfMs = Math.max(0, totalMs - frame.childMs);

    node.renderCount += 1;
    node.totalMs += totalMs;
    node.selfMs += selfMs;
    if (event?.status && event.status !== 'ok') {
      node.errorCount += 1;
    }

    const parentFrame = this.stack.length > 0 ? this.stack[this.stack.length - 1] : null;
    if (parentFrame) {
      parentFrame.childMs += totalMs;
    }

    if (frame.edgeKey && this.edges.has(frame.edgeKey)) {
      const edge = this.edges.get(frame.edgeKey);
      edge.totalMs += totalMs;
      if (event?.status && event.status !== 'ok') {
        edge.errorCount += 1;
      }
    }
  }

  onComponentEvent(event) {
    if (!event || !event.phase) return;
    if (event.phase === 'start') {
      this.startEvent(event);
    } else if (event.phase === 'end') {
      this.endEvent(event);
    }
  }

  onDataFetch(event) {
    if (!event) return;
    const queryRef = Number(event.queryRef ?? 0) >>> 0;
    const signature = Number(event.signature ?? 0) >>> 0;
    const fetchMode = event.fetchMode ? String(event.fetchMode) : 'origin_first_render';
    const callKind = fetchMode === 'edge_delayed' ? 'data_edge_delayed' : 'data_origin_first';
    const queryName = event.name ? String(event.name) : '';
    const durationMs = Number(event.durationMs ?? 0);
    const rowCount = Number(event.rowCount ?? 0);
    const componentCallKind = event.componentCallKind ? String(event.componentCallKind) : 'page';
    const componentPath = event.componentPath ? String(event.componentPath) : '';
    const nodeId = `data:q${queryRef}:s${signature}:${fetchMode}`;
    const nodeLabel = queryName
      ? `q${queryRef} (${queryName})`
      : `q${queryRef}`;
    const node = this.ensureNode({
      nodeType: 'data_fetch',
      path: nodeId,
      label: nodeLabel,
      callKind,
      fetchMode,
      queryRef,
      signature,
      name: queryName,
      renderEngine: componentCallKind === 'page' ? 'page' : 'wasm-vm',
      loadSource: fetchMode === 'edge_delayed' ? 'edge' : 'origin',
      artifactSource: 'origin-data',
      sizeBytes: 0,
    });
    node.fetchCount += 1;
    node.rowCount += Number.isFinite(rowCount) && rowCount > 0 ? rowCount : 0;
    if (Number.isFinite(durationMs) && durationMs >= 0) {
      node.totalMs += durationMs;
      node.selfMs += durationMs;
    }
    if (event.status && String(event.status) !== 'ok') {
      node.errorCount += 1;
    }

    const parentFrame = this.stack.length > 0 ? this.stack[this.stack.length - 1] : null;
    let parentId = parentFrame?.nodeId || componentPath || this.rootId;
    if (!this.nodes.has(parentId)) {
      parentId = this.rootId;
    }
    const edge = this.ensureEdge({
      from: parentId,
      to: node.id,
      edgeType: 'data',
      callKind,
    });
    edge.count += 1;
    edge.fetchModes.add(fetchMode);
    edge.queryRefs.add(queryRef);
    edge.rowCount += Number.isFinite(rowCount) && rowCount > 0 ? rowCount : 0;
    if (Number.isFinite(durationMs) && durationMs >= 0) {
      edge.totalMs += durationMs;
    }
    if (event.status && String(event.status) !== 'ok') {
      edge.errorCount += 1;
    }
  }

  toGraph() {
    const nodes = [];
    for (const node of this.nodes.values()) {
      nodes.push({
        id: node.id,
        nodeType: node.nodeType,
        path: node.path,
        label: node.label,
        callKinds: Array.from(node.callKinds.values()),
        componentOpcodes: Array.from(node.componentOpcodes.values()),
        renderEngines: Array.from(node.renderEngines.values()),
        loadSources: Array.from(node.loadSources.values()),
        artifactSources: Array.from(node.artifactSources.values()),
        fetchModes: Array.from(node.fetchModes.values()),
        queryRefs: Array.from(node.queryRefs.values()),
        signatures: Array.from(node.signatures.values()),
        queryNames: Array.from(node.queryNames.values()),
        sizeBytes: node.sizeBytes,
        renderCount: node.renderCount,
        fetchCount: node.fetchCount,
        rowCount: node.rowCount,
        errorCount: node.errorCount,
        totalMs: node.totalMs,
        selfMs: node.selfMs,
      });
    }
    const edges = Array.from(this.edges.values()).map((edge) => ({
      key: edge.key,
      from: edge.from,
      to: edge.to,
      edgeType: edge.edgeType,
      callKind: edge.callKind,
      componentOpcodes: Array.from(edge.componentOpcodes.values()),
      fetchModes: Array.from(edge.fetchModes.values()),
      queryRefs: Array.from(edge.queryRefs.values()),
      rowCount: edge.rowCount,
      count: edge.count,
      totalMs: edge.totalMs,
      errorCount: edge.errorCount,
    }));
    return {
      rootId: this.rootId,
      nodes,
      edges,
    };
  }
}

function linkingKindLabel(callKind) {
  if (callKind === 'static_origin') return 'Static (Origin)';
  if (callKind === 'dynamic_vm') return 'Dynamic (Edge VM Load)';
  if (callKind === 'linked_vm') return 'Linked (Edge VM)';
  if (callKind === 'linked_edge') return 'Linked (Edge Native)';
  if (callKind === 'data_origin_first') return 'Data (Origin First Render)';
  if (callKind === 'data_edge_delayed') return 'Data (Edge Delayed)';
  if (callKind === 'page') return 'Page Root';
  return callKind || 'unknown';
}

function dataFetchModeColor(fetchModes) {
  const modes = new Set(Array.isArray(fetchModes) ? fetchModes : []);
  if (modes.has('origin_first_render') && modes.has('edge_delayed')) return '#6d28d9';
  if (modes.has('edge_delayed')) return '#c2410c';
  if (modes.has('origin_first_render')) return '#0f766e';
  return '#4b5563';
}

function dataFetchModeLabel(fetchMode) {
  if (fetchMode === 'edge_delayed') return 'Edge Delayed';
  if (fetchMode === 'origin_first_render') return 'Origin First Render';
  return fetchMode || 'unknown';
}

function linkingNodeColor(node) {
  if (node?.nodeType === 'data_fetch') {
    return dataFetchModeColor(node.fetchModes);
  }
  const kinds = new Set(Array.isArray(node?.callKinds) ? node.callKinds : []);
  const opcodes = new Set(Array.isArray(node?.componentOpcodes) ? node.componentOpcodes : []);
  if (kinds.has('page')) return '#334155';
  if (opcodes.has('BC_COMPONENT_LINKED') || kinds.has('linked_vm') || kinds.has('linked_edge')) return '#15803d';
  if (opcodes.has('BC_COMPONENT_LOAD') || kinds.has('dynamic_vm')) return '#b45309';
  if (opcodes.has('BC_COMPONENT_START') || kinds.has('static_origin')) return '#1d4ed8';
  return '#475569';
}

function pageModeHref(pageName, mode) {
  const normalized = String(pageName || 'index').replace(/^\/+|\/+$/g, '');
  const path = normalized && normalized !== 'index' ? `/${normalized}` : '/';
  return `${path}?mot=${encodeURIComponent(String(mode || 'linking'))}`;
}

function buildLinkingFlowEvents({ pageName, trace, graph }) {
  const traceData = trace && typeof trace.toJSON === 'function' ? trace.toJSON() : (trace || {});
  const nodes = Array.isArray(graph?.nodes) ? graph.nodes : [];
  const rootNode = nodes.find((node) => node.id === graph?.rootId) || null;
  const spans = Array.isArray(traceData?.spans) ? traceData.spans : [];
  const componentLoads = Array.isArray(traceData?.componentLoads) ? traceData.componentLoads : [];
  const componentRenders = Array.isArray(traceData?.componentRenders) ? traceData.componentRenders : [];
  const dataFetches = Array.isArray(traceData?.dataFetches) ? traceData.dataFetches : [];
  const dataRequirements = Array.isArray(traceData?.bytecodeDebug?.dataRequirements)
    ? traceData.bytecodeDebug.dataRequirements
    : [];
  const events = [];
  let logicalMs = 0;
  const pushEvent = (lane, title, detail, durationMs, kind) => {
    const duration = Number.isFinite(Number(durationMs)) && Number(durationMs) >= 0 ? Number(durationMs) : 0;
    const startMs = logicalMs;
    logicalMs += duration;
    events.push({
      index: events.length + 1,
      lane,
      title,
      detail,
      durationMs: duration,
      startMs,
      endMs: logicalMs,
      kind: kind || 'event',
    });
  };

  const pageFetch = spans.find((span) => span && span.name === 'page_bytecode_fetch') || null;
  if (pageFetch) {
    pushEvent(
      'origin',
      `Serve page bytecode (${pageName})`,
      `status=${pageFetch.status || 'ok'} | size=${formatBytes(rootNode?.sizeBytes)} | vm=${traceData.vmMode || 'wasm'}`,
      pageFetch.durationMs,
      'artifact',
    );
    pushEvent(
      'edge',
      `Receive page bytecode (${pageName})`,
      `artifact=${graph?.rootId || `pages/${pageName}`} | dependencies=${Array.isArray(traceData.dependencies) ? traceData.dependencies.length : 0}`,
      0.05,
      'artifact',
    );
  }

  for (const load of componentLoads) {
    const source = String(load?.source || 'origin');
    const path = String(load?.path || '(unknown)');
    if (source === 'origin') {
      pushEvent(
        'edge',
        `Request component bytecode (${path})`,
        'type=dynamic component artifact',
        0.05,
        'component_load_request',
      );
      pushEvent(
        'origin',
        `Serve component bytecode (${path})`,
        `status=${load?.status || 'ok'} | size=${formatBytes(load?.sizeBytes)} | source=origin`,
        load?.durationMs,
        'component_load',
      );
    } else {
      pushEvent(
        'edge',
        `Load component artifact (${path})`,
        `status=${load?.status || 'ok'} | size=${formatBytes(load?.sizeBytes)} | source=${source}`,
        load?.durationMs,
        'component_load',
      );
    }
  }

  for (const fetch of dataFetches) {
    const fetchMode = String(fetch?.fetchMode || 'origin_first_render');
    const sourceComponent = String(fetch?.componentPath || `pages/${pageName}`);
    const queryRef = Number(fetch?.queryRef ?? 0) >>> 0;
    const queryName = fetch?.name ? ` (${String(fetch.name)})` : '';
    pushEvent(
      'edge',
      `Request data q${queryRef}${queryName}`,
      `from=${sourceComponent} | stage=${dataFetchModeLabel(fetchMode)}`,
      0.05,
      'data_request',
    );
    pushEvent(
      'origin',
      `Respond data q${queryRef}${queryName}`,
      `rows=${Number(fetch?.rowCount ?? 0)} | status=${fetch?.status || 'ok'} | stage=${dataFetchModeLabel(fetchMode)}`,
      fetch?.durationMs,
      'data_response',
    );
  }

  for (const render of componentRenders) {
    const path = String(render?.path || '(unknown)');
    pushEvent(
      'edge',
      `Render component (${path})`,
      `call=${render?.callKind || 'unknown'} | op=${render?.componentOpcode || 'n/a'} | load=${render?.loadSource || 'unknown'} | engine=${render?.renderEngine || 'unknown'}`,
      render?.durationMs,
      'component_render',
    );
  }

  const originFirst = dataFetches.filter((fetch) => String(fetch?.fetchMode || '') === 'origin_first_render').length;
  const edgeDelayed = dataFetches.filter((fetch) => String(fetch?.fetchMode || '') === 'edge_delayed').length;
  let dataBakeAssessment = 'No runtime data fetch observed.';
  if (dataFetches.length > 0) {
    dataBakeAssessment = `Runtime data fetch observed (${dataFetches.length}); data was not fully baked into shipped bytecode.`;
  } else if (dataRequirements.length > 0) {
    dataBakeAssessment = `No runtime fetch observed for ${dataRequirements.length} declared data requirement(s); data may have been pre-resolved/baked.`;
  }

  return {
    events,
    logicalTotalMs: logicalMs,
    summary: {
      componentLoads: componentLoads.length,
      componentRenders: componentRenders.length,
      dataFetches: dataFetches.length,
      originFirstFetches: originFirst,
      edgeDelayedFetches: edgeDelayed,
      dataRequirements: dataRequirements.length,
      dataBakeAssessment,
    },
  };
}

function renderLinkingFlowPage({ pageName, graph, trace, errorMessage = null }) {
  const esc = escapeHtml;
  const flow = buildLinkingFlowEvents({ pageName, trace, graph });
  const rows = flow.events.map((event) => {
    const laneClass = event.lane === 'origin' ? 'origin' : 'edge';
    const originCell = event.lane === 'origin'
      ? `<div class="event-card ${laneClass}">
  <div class="title">${esc(event.title)}</div>
  <div class="detail">${esc(event.detail)}</div>
</div>`
      : '<div class="empty"></div>';
    const edgeCell = event.lane === 'edge'
      ? `<div class="event-card ${laneClass}">
  <div class="title">${esc(event.title)}</div>
  <div class="detail">${esc(event.detail)}</div>
</div>`
      : '<div class="empty"></div>';
    return `<div class="flow-time">#${event.index}<br/>t=${formatMs(event.endMs)}ms<br/>+${formatMs(event.durationMs)}ms</div>${originCell}${edgeCell}`;
  }).join('\n');

  const errorBanner = errorMessage
    ? `<div class="error">Execution completed with error: ${esc(errorMessage)}</div>`
    : '';

  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Mot Linking Flow - ${esc(pageName)}</title>
  <style>
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; background: #f8fafc; color: #0f172a; }
    .wrap { padding: 18px 20px 28px; max-width: 1400px; margin: 0 auto; }
    h1 { margin: 0 0 6px; font-size: 22px; }
    p { margin: 0 0 10px; color: #334155; }
    .links { margin: 4px 0 12px; font-size: 13px; }
    .links a { color: #1d4ed8; text-decoration: none; font-weight: 600; }
    .links a:hover { text-decoration: underline; }
    .summary { display: grid; grid-template-columns: repeat(3, minmax(220px, 1fr)); gap: 10px; margin: 8px 0 14px; }
    .summary .card { border: 1px solid #cbd5e1; background: #fff; border-radius: 10px; padding: 10px 12px; }
    .summary .k { font-size: 12px; color: #475569; margin-bottom: 4px; }
    .summary .v { font-size: 16px; font-weight: 700; color: #0f172a; }
    .assessment { border: 1px solid #bfdbfe; background: #eff6ff; color: #1e3a8a; border-radius: 10px; padding: 10px 12px; margin-bottom: 14px; }
    .flow { display: grid; grid-template-columns: 120px 1fr 1fr; gap: 8px; align-items: start; }
    .head { font-size: 12px; font-weight: 700; color: #334155; text-transform: uppercase; letter-spacing: .04em; padding: 4px 6px; }
    .flow-time { font: 11px/1.35 ui-monospace, Menlo, Monaco, Consolas, monospace; color: #334155; padding: 8px 6px; border-top: 1px dashed #cbd5e1; }
    .event-card { border-radius: 10px; border: 1px solid #cbd5e1; background: #fff; padding: 9px 11px; border-top: 3px solid #94a3b8; min-height: 60px; }
    .event-card .title { font-weight: 700; font-size: 13px; color: #0f172a; margin-bottom: 4px; }
    .event-card .detail { font-size: 12px; color: #334155; }
    .event-card.origin { border-top-color: #0f766e; background: #f0fdfa; }
    .event-card.edge { border-top-color: #2563eb; background: #eff6ff; }
    .empty { border-top: 1px dashed #cbd5e1; min-height: 78px; }
    .error { margin: 8px 0 14px; padding: 10px 12px; border: 1px solid #dc2626; background: #fef2f2; color: #991b1b; border-radius: 8px; }
    @media (max-width: 980px) {
      .summary { grid-template-columns: 1fr; }
      .flow { grid-template-columns: 1fr; }
      .head:nth-child(2), .head:nth-child(3) { display: none; }
      .empty { display: none; }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Rendering Artifact + Dataflow Timeline</h1>
    <p>Page: <b>${esc(pageName)}</b>. This view preserves linking info but focuses on origin/edge artifacts and query flow over logical time.</p>
    <div class="links"><a href="${esc(pageModeHref(pageName, 'linking'))}">Back to Component Linking Graph</a> | <a href="${esc(pageModeHref(pageName, 'linking-profiler'))}">Open Rendering Profiler View</a></div>
    ${errorBanner}
    <div class="summary">
      <div class="card"><div class="k">Component Loads</div><div class="v">${flow.summary.componentLoads}</div></div>
      <div class="card"><div class="k">Component Renders</div><div class="v">${flow.summary.componentRenders}</div></div>
      <div class="card"><div class="k">Data Fetches</div><div class="v">${flow.summary.dataFetches}</div></div>
      <div class="card"><div class="k">Origin First-Render Fetches</div><div class="v">${flow.summary.originFirstFetches}</div></div>
      <div class="card"><div class="k">Edge Delayed Fetches</div><div class="v">${flow.summary.edgeDelayedFetches}</div></div>
      <div class="card"><div class="k">Declared Data Requirements</div><div class="v">${flow.summary.dataRequirements}</div></div>
    </div>
    <div class="assessment">${esc(flow.summary.dataBakeAssessment)}</div>
    <div class="flow">
      <div class="head">Logical Time</div>
      <div class="head">Origin Lane</div>
      <div class="head">Edge Lane</div>
      ${rows}
    </div>
  </div>
</body>
</html>`;
}

function profilerLabelFromPath(path) {
  const value = String(path || '').trim();
  if (!value) return '(unknown)';
  const idx = value.lastIndexOf('/');
  return idx >= 0 ? value.slice(idx + 1) : value;
}

function profilerColorForSpan(span) {
  const type = String(span?.type || '');
  if (type === 'vm_wait') return '#7c3aed';
  if (type === 'data_fetch') return '#0f766e';
  if (type === 'page') return '#334155';
  if (type === 'browser_wait') return '#64748b';
  const opcode = String(span?.componentOpcode || '');
  const callKind = String(span?.callKind || '');
  if (opcode === 'BC_COMPONENT_LINKED' || callKind === 'linked_vm' || callKind === 'linked_edge') return '#15803d';
  if (opcode === 'BC_COMPONENT_LOAD' || callKind === 'dynamic_vm') return '#b45309';
  return '#1d4ed8';
}

function profilerComponentLaneId(path) {
  return `component:${String(path || '(unknown)')}`;
}

class LinkingProfilerTracker {
  constructor({ pageName }) {
    this.pageName = pageName;
    this.startedAtMs = nowMs();
    this.nextSpanId = 1;
    this.spans = [];
    this.spanById = new Map();
    this.componentFrameStack = [];
    this.waitSpanByReqId = new Map();
    this.waitOwnerBySpanId = new Map();
    this.markers = [];
    this.pageSpanId = null;
    this.browserSpanId = null;
    this.laneMeta = new Map();
    this.ensureLane('origin', { label: 'Origin', color: '#0f766e', kind: 'origin' });
    this.ensureLane('origin:data_fetch', { label: 'Data Fetch', color: '#0f766e', kind: 'origin_data' });
    this.ensureLane('edge_vm', { label: 'VM Wait', color: '#7c3aed', kind: 'edge_vm' });
    this.ensureLane('browser', { label: 'Browser', color: '#64748b', kind: 'browser' });
  }

  ensureLane(id, meta = {}) {
    const laneId = String(id || 'edge');
    if (this.laneMeta.has(laneId)) return laneId;
    this.laneMeta.set(laneId, {
      id: laneId,
      label: meta.label ? String(meta.label) : laneId,
      color: meta.color ? String(meta.color) : '#1d4ed8',
      kind: meta.kind ? String(meta.kind) : 'edge',
    });
    return laneId;
  }

  laneForComponent(path) {
    const normalizedPath = String(path || '(unknown)');
    const laneId = profilerComponentLaneId(normalizedPath);
    this.ensureLane(laneId, {
      label: normalizedPath,
      color: '#1d4ed8',
      kind: 'component',
    });
    return laneId;
  }

  relMs(absoluteMs) {
    const abs = Number.isFinite(Number(absoluteMs)) ? Number(absoluteMs) : nowMs();
    return Math.max(0, abs - this.startedAtMs);
  }

  startSpan(params = {}) {
    const id = `s${this.nextSpanId++}`;
    const startMs = this.relMs(params.atMs);
    const span = {
      id,
      lane: params.lane || 'edge',
      type: params.type || 'span',
      label: params.label ? String(params.label) : '(span)',
      startMs,
      endMs: null,
      durationMs: null,
      depth: Number.isFinite(Number(params.depth)) ? Math.max(0, Number(params.depth)) : 0,
      status: params.status ? String(params.status) : 'ok',
      path: params.path ? String(params.path) : '',
      callKind: params.callKind ? String(params.callKind) : '',
      componentOpcode: params.componentOpcode ? String(params.componentOpcode) : '',
      visible: params.visible !== false,
      detail: params.detail && typeof params.detail === 'object' ? { ...params.detail } : {},
    };
    this.spans.push(span);
    this.spanById.set(id, span);
    return id;
  }

  endSpan(spanId, params = {}) {
    const span = spanId ? this.spanById.get(spanId) : null;
    if (!span || span.endMs != null) return null;
    const endMs = this.relMs(params.atMs);
    span.endMs = Math.max(span.startMs, endMs);
    span.durationMs = Math.max(0, span.endMs - span.startMs);
    if (params.status) span.status = String(params.status);
    if (params.callKind) span.callKind = String(params.callKind);
    if (params.componentOpcode) span.componentOpcode = String(params.componentOpcode);
    if (params.detail && typeof params.detail === 'object') {
      span.detail = { ...span.detail, ...params.detail };
    }
    if (span.type === 'component' || span.type === 'component_total' || span.type === 'page') {
      const blockedWallMs = Number(span.detail?.blockedWallMs ?? span.detail?.vmWaitMs ?? 0);
      span.detail.blockedWallMs = Math.max(0, Number.isFinite(blockedWallMs) ? blockedWallMs : 0);
      span.detail.activeWallMs = Math.max(0, span.durationMs - span.detail.blockedWallMs);
    }
    return span;
  }

  addBlockedTime(ownerSpanId, blockedMs) {
    if (!ownerSpanId) return;
    const owner = this.spanById.get(ownerSpanId);
    if (!owner) return;
    const value = Number(blockedMs ?? 0);
    if (!Number.isFinite(value) || value <= 0) return;
    const current = Number(owner.detail?.blockedWallMs ?? owner.detail?.vmWaitMs ?? 0);
    const next = Math.max(0, (Number.isFinite(current) ? current : 0) + value);
    owner.detail = { ...owner.detail, blockedWallMs: next, vmWaitMs: next };
  }

  addMarker(params = {}) {
    this.markers.push({
      lane: params.lane || 'edge',
      kind: params.kind || 'marker',
      timeMs: this.relMs(params.atMs),
      label: params.label ? String(params.label) : '',
      detail: params.detail ? String(params.detail) : '',
    });
  }

  begin() {
    const pagePath = `pages/${this.pageName}`;
    const pageLane = this.laneForComponent(pagePath);
    this.pageSpanId = this.startSpan({
      lane: pageLane,
      type: 'page',
      label: profilerLabelFromPath(pagePath),
      path: pagePath,
      atMs: this.startedAtMs,
      visible: false,
    });
    this.browserSpanId = this.startSpan({
      lane: 'browser',
      type: 'browser_wait',
      label: 'Browser waiting response',
      atMs: this.startedAtMs,
    });
  }

  end(status = 'ok', errorMessage = null) {
    const atMs = nowMs();
    while (this.componentFrameStack.length > 0) {
      const frame = this.componentFrameStack.pop();
      this.pauseComponentExecution(frame, atMs, 'teardown');
      this.endSpan(frame.totalSpanId, {
        atMs,
        status,
      });
    }
    for (const spanIds of this.waitSpanByReqId.values()) {
      for (const spanId of spanIds) {
        const waitSpan = this.endSpan(spanId, { atMs, status: 'incomplete' });
        const ownerSpanId = this.waitOwnerBySpanId.get(spanId) || null;
        if (waitSpan) {
          this.addBlockedTime(ownerSpanId, waitSpan.durationMs);
        }
        this.waitOwnerBySpanId.delete(spanId);
      }
    }
    this.waitSpanByReqId.clear();
    this.endSpan(this.pageSpanId, {
      atMs,
      status,
      detail: errorMessage ? { error: errorMessage } : {},
    });
    this.endSpan(this.browserSpanId, {
      atMs,
      status,
      detail: {
        outcome: status === 'ok' ? 'response_received' : 'error_response',
      },
    });
  }

  pauseComponentExecution(frame, atMs, reason = 'pause') {
    if (!frame || !frame.activeSpanId) return;
    this.endSpan(frame.activeSpanId, {
      atMs,
      detail: {
        pauseReason: String(reason),
      },
    });
    frame.activeSpanId = null;
  }

  resumeComponentExecution(frame, atMs, reason = 'resume') {
    if (!frame || frame.activeSpanId) return;
    frame.segmentIndex = (Number(frame.segmentIndex ?? 0) + 1);
    frame.activeSpanId = this.startSpan({
      lane: frame.lane,
      type: 'component',
      label: profilerLabelFromPath(frame.path),
      path: frame.path,
      callKind: frame.callKind,
      componentOpcode: frame.componentOpcode,
      atMs,
      detail: {
        segmentIndex: frame.segmentIndex,
        resumeReason: String(reason),
        renderEngine: frame.renderEngine || 'wasm-vm',
        loadSource: frame.loadSource || 'unknown',
        artifactSource: frame.artifactSource || 'unknown',
        sizeBytes: Number(frame.sizeBytes ?? 0),
      },
    });
  }

  onComponentEvent(event) {
    if (!event || !event.phase) return;
    const atMs = nowMs();
    const phase = String(event.phase);
    const path = String(event.path || '');
    if (phase === 'start') {
      const parentFrame = this.componentFrameStack.length > 0
        ? this.componentFrameStack[this.componentFrameStack.length - 1]
        : null;
      if (parentFrame) {
        this.pauseComponentExecution(parentFrame, atMs, 'child_component');
      }
      const lane = this.laneForComponent(path);
      const totalSpanId = this.startSpan({
        lane,
        type: 'component_total',
        label: profilerLabelFromPath(path),
        path,
        callKind: event.callKind,
        componentOpcode: event.componentOpcode,
        atMs,
        visible: false,
        detail: {
          renderEngine: event.renderEngine || 'wasm-vm',
          loadSource: event.loadSource || 'unknown',
          artifactSource: event.artifactSource || 'unknown',
          sizeBytes: Number(event.sizeBytes ?? 0),
        },
      });
      const frame = {
        path,
        lane,
        totalSpanId,
        activeSpanId: null,
        segmentIndex: 0,
        callKind: event.callKind || 'static_origin',
        componentOpcode: event.componentOpcode || 'BC_COMPONENT_START',
        renderEngine: event.renderEngine || 'wasm-vm',
        loadSource: event.loadSource || 'unknown',
        artifactSource: event.artifactSource || 'unknown',
        sizeBytes: Number(event.sizeBytes ?? 0),
      };
      this.componentFrameStack.push(frame);
      this.resumeComponentExecution(frame, atMs, 'component_start');
      return;
    }

    if (phase === 'end') {
      let frameIndex = -1;
      for (let i = this.componentFrameStack.length - 1; i >= 0; i--) {
        if (this.componentFrameStack[i].path === path) {
          frameIndex = i;
          break;
        }
      }
      if (frameIndex < 0) return;
      const wasTop = frameIndex === this.componentFrameStack.length - 1;
      const frame = this.componentFrameStack[frameIndex];
      this.pauseComponentExecution(frame, atMs, 'component_end');
      this.componentFrameStack.splice(frameIndex, 1);
      this.endSpan(frame.totalSpanId, {
        atMs,
        status: event.status || 'ok',
        callKind: event.callKind,
        componentOpcode: event.componentOpcode,
        detail: {
          durationMsReported: Number(event.durationMs ?? 0),
          renderEngine: event.renderEngine || 'wasm-vm',
          loadSource: event.loadSource || 'unknown',
          artifactSource: event.artifactSource || 'unknown',
          sizeBytes: Number(event.sizeBytes ?? 0),
          error: event.error || null,
        },
      });
      if (wasTop && this.componentFrameStack.length > 0) {
        this.resumeComponentExecution(
          this.componentFrameStack[this.componentFrameStack.length - 1],
          atMs,
          'child_return',
        );
      }
    }
  }

  onVmAwait(event) {
    const reqId = Number(event?.reqId ?? 0) >>> 0;
    const atMs = event?.atMs;
    const queryRef = Number(event?.queryRef ?? 0) >>> 0;
    const waitLabel = event?.kind === 'data_fetch'
      ? `VM wait q${queryRef}`
      : `VM wait ${profilerLabelFromPath(event?.componentPath || event?.name || 'component')}`;
    const ownerFrame = this.componentFrameStack.length > 0
      ? this.componentFrameStack[this.componentFrameStack.length - 1]
      : null;
    if (ownerFrame) {
      this.pauseComponentExecution(ownerFrame, atMs, 'io_wait');
    }
    const waitSpanId = this.startSpan({
      lane: 'edge_vm',
      type: 'vm_wait',
      label: waitLabel,
      atMs,
      depth: this.componentFrameStack.length,
      detail: {
        kind: event?.kind || 'await',
        queryRef,
        signature: Number(event?.signature ?? 0) >>> 0,
        name: event?.name || '',
        componentPath: event?.componentPath || '',
        componentCallKind: event?.componentCallKind || 'page',
      },
    });
    const waitOwnerSpanId = ownerFrame?.totalSpanId || this.pageSpanId;
    if (waitOwnerSpanId) {
      this.waitOwnerBySpanId.set(waitSpanId, waitOwnerSpanId);
      const ownerSpan = this.spanById.get(waitOwnerSpanId);
      if (ownerSpan) {
        const currentDetail = ownerSpan.detail && typeof ownerSpan.detail === 'object' ? ownerSpan.detail : {};
        ownerSpan.detail = {
          ...currentDetail,
          vmWaitCount: Number(currentDetail.vmWaitCount ?? 0) + 1,
        };
      }
    }
    if (!this.waitSpanByReqId.has(reqId)) {
      this.waitSpanByReqId.set(reqId, []);
    }
    this.waitSpanByReqId.get(reqId).push(waitSpanId);
    this.addMarker({
      lane: 'edge_vm',
      kind: 'vm_pause',
      atMs,
      label: 'pause',
      detail: waitLabel,
    });
  }

  onVmResume(event) {
    const reqId = Number(event?.reqId ?? 0) >>> 0;
    const atMs = event?.atMs;
    const spanIds = this.waitSpanByReqId.get(reqId) || [];
    const spanId = spanIds.length > 0 ? spanIds.pop() : null;
    if (spanId) {
      const waitSpan = this.endSpan(spanId, {
        atMs,
        status: 'ok',
      });
      const ownerSpanId = this.waitOwnerBySpanId.get(spanId) || null;
      if (waitSpan) {
        this.addBlockedTime(ownerSpanId, waitSpan.durationMs);
      }
      this.waitOwnerBySpanId.delete(spanId);
      if (spanIds.length === 0) {
        this.waitSpanByReqId.delete(reqId);
      }
    }
    if (this.componentFrameStack.length > 0) {
      this.resumeComponentExecution(
        this.componentFrameStack[this.componentFrameStack.length - 1],
        atMs,
        'io_resumed',
      );
    }
    this.addMarker({
      lane: 'edge_vm',
      kind: 'vm_resume',
      atMs,
      label: 'resume',
      detail: `req=${reqId}`,
    });
  }

  onDataFetch(event) {
    if (!event) return;
    const queryRef = Number(event.queryRef ?? 0) >>> 0;
    const queryName = event.name ? ` (${String(event.name)})` : '';
    const spanId = this.startSpan({
      lane: 'origin:data_fetch',
      type: 'data_fetch',
      label: `q${queryRef}${queryName}`,
      atMs: event.startedAtMs,
      detail: {
        fetchMode: event.fetchMode || 'origin_first_render',
        componentPath: event.componentPath || '',
        componentCallKind: event.componentCallKind || 'page',
        rowCount: Number(event.rowCount ?? 0),
        status: event.status || 'ok',
        responseReceivedAtMs: this.relMs(event.responseReceivedAtMs),
        parseMs: Number(event.parseMs ?? 0),
      },
    });
    this.endSpan(spanId, {
      atMs: event.endedAtMs,
      status: event.status || 'ok',
      detail: {
        durationMsReported: Number(event.durationMs ?? 0),
      },
    });
  }

  toJSON() {
    const finalizedSpans = this.spans
      .map((span) => {
        const endMs = span.endMs == null ? span.startMs : span.endMs;
        const durationMs = span.durationMs == null ? Math.max(0, endMs - span.startMs) : span.durationMs;
        return {
          ...span,
          endMs,
          durationMs,
        };
      })
      .sort((a, b) => a.startMs - b.startMs || a.depth - b.depth || String(a.label).localeCompare(String(b.label)));
    const totalMs = finalizedSpans.reduce((max, span) => Math.max(max, span.endMs), 0);
    const laneMeta = {};
    for (const [id, meta] of this.laneMeta.entries()) {
      laneMeta[id] = { ...meta };
    }
    const lanePriority = (id) => {
      if (id === 'origin') return 0;
      if (String(id).startsWith('component:')) return 1;
      if (id === 'edge_vm') return 2;
      if (id === 'browser') return 3;
      return 4;
    };
    const lanes = Object.keys(laneMeta)
      .sort((a, b) => lanePriority(a) - lanePriority(b) || String(laneMeta[a]?.label || a).localeCompare(String(laneMeta[b]?.label || b)));
    return {
      pageName: this.pageName,
      totalMs,
      lanes,
      laneMeta,
      spans: finalizedSpans,
      markers: this.markers.slice().sort((a, b) => a.timeMs - b.timeMs),
    };
  }
}

function renderLinkingProfilerPage({ pageName, profile, errorMessage = null }) {
  const esc = escapeHtml;
  const spans = Array.isArray(profile?.spans) ? profile.spans : [];
  const markers = Array.isArray(profile?.markers) ? profile.markers : [];
  const profileLanes = Array.isArray(profile?.lanes) ? profile.lanes.map((lane) => String(lane)) : [];
  const profileLaneMeta = profile?.laneMeta && typeof profile.laneMeta === 'object' ? profile.laneMeta : {};
  const laneSet = new Set(profileLanes);
  const laneActivity = Object.create(null);
  for (const span of spans) {
    const lane = String(span?.lane || '');
    if (lane) laneSet.add(lane);
    if (lane && span?.visible !== false) {
      laneActivity[lane] = (laneActivity[lane] || 0) + 1;
    }
  }
  for (const marker of markers) {
    const lane = String(marker?.lane || '');
    if (lane) laneSet.add(lane);
    if (lane) {
      laneActivity[lane] = (laneActivity[lane] || 0) + 1;
    }
  }
  laneSet.add('origin:data_fetch');
  laneSet.add('edge_vm');
  laneSet.add('browser');
  const majorForLane = (lane) => {
    if (lane === 'browser' || lane.startsWith('browser')) return 'browser';
    if (lane === 'origin' || lane.startsWith('origin')) return 'origin';
    return 'edge';
  };
  const sublaneRank = (lane) => {
    const meta = profileLaneMeta[lane] && typeof profileLaneMeta[lane] === 'object'
      ? profileLaneMeta[lane]
      : null;
    const kind = String(meta?.kind || '');
    if (kind === 'origin') return 0;
    if (kind === 'origin_data') return 1;
    if (kind === 'component') return 0;
    if (kind === 'edge_vm') return 1;
    if (kind === 'browser') return 0;
    return 5;
  };
  const lanes = Array.from(laneSet.values())
    .filter((lane) => !(lane === 'origin' && !laneActivity[lane]))
    .sort((a, b) => {
    const majorOrderMap = { origin: 0, edge: 1, browser: 2 };
    const p = (majorOrderMap[majorForLane(a)] ?? 9) - (majorOrderMap[majorForLane(b)] ?? 9);
    if (p !== 0) return p;
    const sp = sublaneRank(a) - sublaneRank(b);
    if (sp !== 0) return sp;
    return String(a).localeCompare(String(b));
  });
  const laneLabel = {};
  const laneColor = {};
  for (const lane of lanes) {
    const meta = profileLaneMeta[lane] && typeof profileLaneMeta[lane] === 'object'
      ? profileLaneMeta[lane]
      : null;
    if (meta?.label) {
      laneLabel[lane] = String(meta.label);
    } else if (lane === 'origin') {
      laneLabel[lane] = 'Origin';
    } else if (lane === 'origin:data_fetch') {
      laneLabel[lane] = 'Data Fetch';
    } else if (lane === 'edge_vm') {
      laneLabel[lane] = 'VM Wait';
    } else if (lane === 'browser') {
      laneLabel[lane] = 'Browser';
    } else if (lane.startsWith('component:')) {
      laneLabel[lane] = lane.slice('component:'.length);
    } else {
      laneLabel[lane] = lane;
    }
    if (meta?.color) {
      laneColor[lane] = String(meta.color);
    } else if (lane === 'origin') {
      laneColor[lane] = '#0f766e';
    } else if (lane === 'origin:data_fetch') {
      laneColor[lane] = '#0f766e';
    } else if (lane === 'edge_vm') {
      laneColor[lane] = '#7c3aed';
    } else if (lane === 'browser') {
      laneColor[lane] = '#64748b';
    } else {
      laneColor[lane] = '#1d4ed8';
    }
  }
  const laneLookup = new Set(lanes);
  const totalMsRaw = Number(profile?.totalMs ?? 0);
  const totalMs = totalMsRaw > 0 ? totalMsRaw : 1;
  const maxLaneLabelLen = lanes.reduce(
    (max, lane) => Math.max(max, String(laneLabel[lane] || lane).length),
    0,
  );
  const leftPad = Math.max(180, Math.min(460, 36 + maxLaneLabelLen * 6));
  const rightPad = 40;
  const topPad = 28;
  const majorHeaderH = 18;
  const majorGap = 14;
  const sublaneGap = 2;
  const trackH = 12;
  const trackGap = 1;
  const normalizedSpans = spans.map((span, index) => {
    const rawLane = String(span?.lane || '');
    const lane = laneLookup.has(rawLane) ? rawLane : 'origin';
    const depth = Math.max(0, Number(span?.depth ?? 0));
    const startMs = Math.max(0, Number(span?.startMs ?? 0));
    const rawEndMs = Number(span?.endMs ?? startMs);
    const endMs = Math.max(startMs, rawEndMs);
    return {
      id: span?.id ? String(span.id) : `s_auto_${index + 1}`,
      lane,
      depth,
      type: span?.type ? String(span.type) : 'span',
      label: span?.label ? String(span.label) : '(span)',
      path: span?.path ? String(span.path) : '',
      callKind: span?.callKind ? String(span.callKind) : '',
      componentOpcode: span?.componentOpcode ? String(span.componentOpcode) : '',
      status: span?.status ? String(span.status) : 'ok',
      startMs,
      endMs,
      durationMs: Math.max(0, endMs - startMs),
      row: 0,
      visible: span?.visible !== false,
      detail: span?.detail && typeof span.detail === 'object' ? span.detail : {},
      color: profilerColorForSpan(span),
    };
  });
  const laneDepth = {};
  const laneItems = {};
  const laneRows = {};
  const laneHeight = {};
  for (const lane of lanes) {
    laneDepth[lane] = 0;
    laneItems[lane] = [];
    laneRows[lane] = 1;
    laneHeight[lane] = trackH;
  }
  for (let i = 0; i < normalizedSpans.length; i++) {
    const span = normalizedSpans[i];
    if (span.visible === false) continue;
    if (!laneItems[span.lane]) laneItems[span.lane] = [];
    laneItems[span.lane].push({
      idx: i,
      startMs: span.startMs,
      endMs: span.endMs,
    });
  }
  for (const lane of lanes) {
    const items = laneItems[lane] || [];
    items.sort((a, b) => a.startMs - b.startMs || b.endMs - a.endMs || a.idx - b.idx);
    const rowEndTimes = [];
    for (const item of items) {
      let row = -1;
      for (let i = 0; i < rowEndTimes.length; i++) {
        if (rowEndTimes[i] <= item.startMs + 0.000001) {
          row = i;
          break;
        }
      }
      if (row < 0) {
        row = rowEndTimes.length;
        rowEndTimes.push(item.endMs);
      } else {
        rowEndTimes[row] = Math.max(rowEndTimes[row], item.endMs);
      }
      normalizedSpans[item.idx].row = row;
      laneDepth[lane] = Math.max(laneDepth[lane], row);
    }
    laneRows[lane] = laneDepth[lane] + 1;
    laneHeight[lane] = laneRows[lane] * trackH + Math.max(0, laneRows[lane] - 1) * trackGap;
  }

  const majorLabel = {
    origin: 'Origin',
    edge: 'Edge',
    browser: 'Browser',
  };
  const majorColor = {
    origin: '#0f766e',
    edge: '#1d4ed8',
    browser: '#64748b',
  };
  const majorOrder = ['origin', 'edge', 'browser'];
  const majorLanes = {};
  for (const major of majorOrder) {
    majorLanes[major] = lanes.filter((lane) => majorForLane(lane) === major);
    majorLanes[major].sort((a, b) => {
      const p = sublaneRank(a) - sublaneRank(b);
      if (p !== 0) return p;
      return String(laneLabel[a] || a).localeCompare(String(laneLabel[b] || b));
    });
  }

  const laneTop = {};
  const majorTop = {};
  const majorHeight = {};
  let yCursor = topPad;
  for (const major of majorOrder) {
    majorTop[major] = yCursor;
    const sublanes = majorLanes[major] || [];
    let y = yCursor + majorHeaderH;
    if (sublanes.length === 0) {
      majorHeight[major] = majorHeaderH + trackH;
      yCursor += majorHeight[major] + majorGap;
      continue;
    }
    for (const lane of sublanes) {
      laneTop[lane] = y;
      y += laneHeight[lane] + sublaneGap;
    }
    y -= sublaneGap;
    majorHeight[major] = Math.max(majorHeaderH + trackH, y - yCursor);
    yCursor = yCursor + majorHeight[major] + majorGap;
  }
  const svgHeight = yCursor + 16;
  const initialScale = Math.max(12, Math.min(40, 1800 / totalMs));
  const initialPlotWidth = Math.max(1000, totalMs * initialScale);
  const normalizedMarkers = markers.map((marker) => ({
    lane: laneLookup.has(String(marker?.lane || '')) ? String(marker.lane) : 'edge_vm',
    kind: marker?.kind ? String(marker.kind) : 'marker',
    label: marker?.label ? String(marker.label) : '',
    detail: marker?.detail ? String(marker.detail) : '',
    timeMs: Math.max(0, Number(marker?.timeMs ?? 0)),
  }));
  const spansJson = JSON.stringify(normalizedSpans).replace(/</g, '\\u003c');
  const markersJson = JSON.stringify(normalizedMarkers).replace(/</g, '\\u003c');
  const cfgJson = JSON.stringify({
    lanes,
    majorOrder,
    majorLabel,
    majorColor,
    majorLanes,
    majorTop,
    majorHeight,
    laneLabel,
    laneColor,
    laneRows,
    laneHeight,
    laneTop,
    totalMs,
    leftPad,
    rightPad,
    topPad,
    trackH,
    trackGap,
    svgHeight,
  }).replace(/</g, '\\u003c');

  const errorBanner = errorMessage
    ? `<div class="error">Execution completed with error: ${esc(errorMessage)}</div>`
    : '';

  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Mot Linking Profiler - ${esc(pageName)}</title>
  <style>
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; background: #f8fafc; color: #0f172a; }
    .wrap { padding: 18px 20px 26px; }
    h1 { margin: 0 0 6px; font-size: 22px; }
    p { margin: 0 0 10px; color: #334155; }
    .links { margin: 4px 0 12px; font-size: 13px; }
    .links a { color: #1d4ed8; text-decoration: none; font-weight: 600; margin-right: 12px; }
    .links a:hover { text-decoration: underline; }
    .legend { display: flex; gap: 12px; flex-wrap: wrap; margin: 8px 0 12px; font-size: 12px; color: #334155; }
    .legend span::before { content: ''; display: inline-block; width: 10px; height: 10px; border-radius: 3px; margin-right: 6px; vertical-align: middle; }
    .k-static::before { background: #1d4ed8; }
    .k-linked::before { background: #15803d; }
    .k-dynamic::before { background: #b45309; }
    .k-wait::before { background: #7c3aed; }
    .k-data::before { background: #0f766e; }
    .k-browser::before { background: #64748b; }
    .meta { display: flex; gap: 14px; align-items: center; flex-wrap: wrap; margin: 4px 0 8px; font-size: 12px; color: #334155; }
    .meta b { color: #0f172a; }
    .meta button { border: 1px solid #cbd5e1; background: #fff; border-radius: 7px; padding: 4px 8px; font-size: 12px; cursor: pointer; }
    .meta button:hover { background: #f8fafc; }
    .meta-note { margin: 0 0 8px; font-size: 12px; color: #475569; }
    .canvas { border: 1px solid #cbd5e1; background: #fff; border-radius: 10px; overflow: hidden; box-shadow: 0 8px 20px rgba(15, 23, 42, 0.08); }
    svg { display: block; }
    .details { margin-top: 12px; border: 1px solid #cbd5e1; background: #fff; border-radius: 10px; padding: 10px 12px; }
    .details h2 { margin: 0 0 6px; font-size: 14px; }
    .details pre { margin: 0; white-space: pre-wrap; font: 12px/1.35 ui-monospace, Menlo, Monaco, Consolas, monospace; color: #0f172a; }
    .error { margin: 8px 0 14px; padding: 10px 12px; border: 1px solid #dc2626; background: #fef2f2; color: #991b1b; border-radius: 8px; }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Rendering Profiler Timeline</h1>
    <p>Time is on the X axis. Y axis has major lanes (Origin, Edge, Browser) with compact sublanes for components, VM waits, and data fetches.</p>
    <div class="links">
      <a href="${esc(pageModeHref(pageName, 'linking'))}">Component Linking Graph</a>
      <a href="${esc(pageModeHref(pageName, 'linking-flow'))}">Artifact + Dataflow Timeline</a>
    </div>
    ${errorBanner}
    <div class="legend">
      <span class="k-static">Static Component</span>
      <span class="k-linked">Linked Component</span>
      <span class="k-dynamic">Dynamic Component</span>
      <span class="k-wait">VM Pause/Wait</span>
      <span class="k-data">Origin Data Fetch</span>
      <span class="k-browser">Browser Wait</span>
    </div>
    <div class="meta">
      <span><b>Total:</b> <span id="mot-profiler-total-ms">0.0ms</span></span>
      <span><b>Window:</b> <span id="mot-profiler-window">0.0ms - 0.0ms</span></span>
      <span><b>Zoom:</b> <span id="mot-profiler-zoom">1.0x</span></span>
      <span>Scroll to zoom on X axis at cursor position.</span>
      <button id="mot-profiler-reset" type="button">Reset Zoom</button>
    </div>
    <p class="meta-note">Timing source: <code>performance.now()</code> wall-clock. It is not CPU counters. Component detail includes <code>activeWallMs</code> and <code>blockedWallMs</code> (VM waits).</p>
    <div class="canvas" id="mot-profiler-canvas">
      <svg id="mot-profiler-svg" width="${leftPad + initialPlotWidth + rightPad}" height="${svgHeight}" viewBox="0 0 ${leftPad + initialPlotWidth + rightPad} ${svgHeight}" xmlns="http://www.w3.org/2000/svg"></svg>
    </div>
    <div class="details">
      <h2>Selection</h2>
      <pre id="mot-profiler-details">Select a span to inspect metadata.</pre>
    </div>
  </div>
  <script>
    (function () {
      var spans = ${spansJson};
      var markers = ${markersJson};
      var cfg = ${cfgJson};
      var lanes = Array.isArray(cfg.lanes) ? cfg.lanes : ['origin:data_fetch', 'edge_vm', 'browser'];
      var majorOrder = Array.isArray(cfg.majorOrder) ? cfg.majorOrder : ['origin', 'edge', 'browser'];
      var map = Object.create(null);
      for (var i = 0; i < spans.length; i++) map[spans[i].id] = spans[i];
      var maxMs = Math.max(1, Number(cfg.totalMs) || 1);
      var minWindowMs = Math.max(0.05, maxMs / 500);
      var viewMinMs = 0;
      var viewMaxMs = maxMs;
      var selectedSpanId = null;
      for (var si = 0; si < spans.length; si++) {
        if (spans[si].visible === false) continue;
        selectedSpanId = spans[si].id;
        break;
      }

      var canvas = document.getElementById('mot-profiler-canvas');
      var svg = document.getElementById('mot-profiler-svg');
      var detailEl = document.getElementById('mot-profiler-details');
      var totalEl = document.getElementById('mot-profiler-total-ms');
      var windowEl = document.getElementById('mot-profiler-window');
      var zoomEl = document.getElementById('mot-profiler-zoom');
      var resetBtn = document.getElementById('mot-profiler-reset');

      function clamp(v, lo, hi) {
        return Math.min(hi, Math.max(lo, v));
      }

      function text(v) {
        return v == null ? '' : String(v);
      }

      function fmt(ms) {
        var n = Number(ms);
        if (!Number.isFinite(n) || n < 0) return '0.0';
        if (n >= 100) return n.toFixed(1);
        if (n >= 10) return n.toFixed(2);
        return n.toFixed(3);
      }

      function escXml(value) {
        return String(value == null ? '' : value)
          .replace(/&/g, '&amp;')
          .replace(/</g, '&lt;')
          .replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;')
          .replace(/'/g, '&#39;');
      }

      function shortLabel(value, maxLen) {
        var str = String(value == null ? '' : value);
        var limit = Number(maxLen || 30);
        if (str.length <= limit) return str;
        return str.slice(0, Math.max(3, limit - 1)) + '…';
      }

      function tickStep(rangeMs) {
        var candidates = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000];
        var rough = rangeMs / 10;
        for (var i = 0; i < candidates.length; i++) {
          if (candidates[i] >= rough) return candidates[i];
        }
        return candidates[candidates.length - 1];
      }

      function rangeMs() {
        return Math.max(0.000001, viewMaxMs - viewMinMs);
      }

      function mapX(ms, plotWidth) {
        return cfg.leftPad + ((ms - viewMinMs) / rangeMs()) * plotWidth;
      }

      function show(id) {
        var s = map[id];
        if (!s || !detailEl) return;
        var lines = [
          'id: ' + text(s.id),
          'label: ' + text(s.label),
          'lane: ' + text(s.lane),
          'row: ' + text(s.row),
          'type: ' + text(s.type),
          'path: ' + text(s.path || ''),
          'callKind: ' + text(s.callKind || ''),
          'opcode: ' + text(s.componentOpcode || ''),
          'status: ' + text(s.status || ''),
          'start: ' + text(fmt(s.startMs)) + 'ms',
          'end: ' + text(fmt(s.endMs)) + 'ms',
          'duration: ' + text(fmt(s.durationMs)) + 'ms',
          'activeWallMs: ' + text(fmt(Number((s.detail && s.detail.activeWallMs) || 0))) + 'ms',
          'blockedWallMs: ' + text(fmt(Number((s.detail && (s.detail.blockedWallMs || s.detail.vmWaitMs)) || 0))) + 'ms',
          'depth: ' + text(s.depth),
          'detail: ' + JSON.stringify(s.detail || {}, null, 2),
        ];
        detailEl.textContent = lines.join('\\n');
      }

      function render() {
        if (!svg || !canvas) return;
        var canvasWidth = Math.max(900, canvas.clientWidth || 900);
        var plotWidth = Math.max(500, canvasWidth - cfg.leftPad - cfg.rightPad);
        var svgWidth = cfg.leftPad + plotWidth + cfg.rightPad;
        svg.setAttribute('width', String(svgWidth));
        svg.setAttribute('height', String(cfg.svgHeight));
        svg.setAttribute('viewBox', '0 0 ' + svgWidth + ' ' + cfg.svgHeight);

        var out = [];
        for (var mi = 0; mi < majorOrder.length; mi++) {
          var major = majorOrder[mi];
          var mTop = Number(cfg.majorTop[major] || 0);
          var mHeight = Number(cfg.majorHeight[major] || 0);
          if (mHeight <= 0) continue;
          var mLabel = cfg.majorLabel[major] || major;
          var mColor = cfg.majorColor[major] || '#334155';
          var mFill = major === 'origin'
            ? '#f0fdfa'
            : (major === 'edge' ? '#eff6ff' : '#f8fafc');
          out.push('<g>');
          out.push('<rect x="0" y="' + mTop + '" width="' + svgWidth + '" height="' + mHeight + '" fill="' + mFill + '" stroke="#e2e8f0" />');
          out.push('<rect x="0" y="' + mTop + '" width="' + cfg.leftPad + '" height="' + mHeight + '" fill="#f8fafc" />');
          out.push('<text x="10" y="' + (mTop + 13) + '" fill="' + escXml(mColor) + '" font-size="12" font-weight="700">' + escXml(mLabel) + '</text>');
          var sublanes = Array.isArray(cfg.majorLanes[major]) ? cfg.majorLanes[major] : [];
          for (var li = 0; li < sublanes.length; li++) {
            var lane = sublanes[li];
            var ly = Number(cfg.laneTop[lane] || 0);
            var lh = Number(cfg.laneHeight[lane] || cfg.trackH || 12);
            out.push('<line x1="0" y1="' + ly + '" x2="' + svgWidth + '" y2="' + ly + '" stroke="#e2e8f0" stroke-width="1" />');
            out.push('<rect x="' + cfg.leftPad + '" y="' + ly + '" width="' + (svgWidth - cfg.leftPad) + '" height="' + lh + '" fill="#ffffff" />');
            out.push('<text x="20" y="' + (ly + Math.min(10, lh - 2)) + '" fill="' + escXml(cfg.laneColor[lane] || '#334155') + '" font-size="10" font-weight="600">' + escXml(shortLabel(cfg.laneLabel[lane] || lane, 34)) + '</text>');
          }
          out.push('</g>');
        }

        var step = tickStep(rangeMs());
        var tickStart = Math.ceil(viewMinMs / step) * step;
        for (var t = tickStart; t <= viewMaxMs + step * 0.25; t += step) {
          var x = mapX(t, plotWidth);
          out.push('<g>');
          out.push('<line x1="' + x + '" y1="' + cfg.topPad + '" x2="' + x + '" y2="' + (cfg.svgHeight - 10) + '" stroke="#e2e8f0" stroke-width="1" />');
          out.push('<text x="' + (x + 2) + '" y="' + (cfg.topPad - 8) + '" font-size="10" fill="#64748b">' + escXml(fmt(t)) + 'ms</text>');
          out.push('</g>');
        }

        for (var m = 0; m < markers.length; m++) {
          var marker = markers[m];
          var markerTime = Number(marker.timeMs || 0);
          if (markerTime < viewMinMs || markerTime > viewMaxMs) continue;
          var laneForMarker = lanes.indexOf(marker.lane) >= 0 ? marker.lane : 'edge_vm';
          var mx = mapX(markerTime, plotWidth);
          var markerLaneTop = Number(cfg.laneTop[laneForMarker] || 0);
          var markerLaneHeight = Number(cfg.laneHeight[laneForMarker] || cfg.trackH || 12);
          var y1 = markerLaneTop - 1;
          var y2 = markerLaneTop + markerLaneHeight + 1;
          var markerColor = marker.kind === 'vm_pause' ? '#7c3aed' : '#14b8a6';
          out.push('<g>');
          out.push('<line x1="' + mx + '" y1="' + y1 + '" x2="' + mx + '" y2="' + y2 + '" stroke="' + markerColor + '" stroke-width="1.5" stroke-dasharray="4 3" />');
          out.push('<text x="' + (mx + 4) + '" y="' + (y1 - 2) + '" font-size="9" fill="' + markerColor + '">' + escXml(marker.label || marker.kind || 'marker') + '</text>');
          out.push('</g>');
        }

        for (var s = 0; s < spans.length; s++) {
          var span = spans[s];
          if (span.visible === false) continue;
          if (span.endMs < viewMinMs || span.startMs > viewMaxMs) continue;
          var laneForSpan = lanes.indexOf(span.lane) >= 0 ? span.lane : 'origin';
          var visStart = Math.max(viewMinMs, span.startMs);
          var visEnd = Math.min(viewMaxMs, span.endMs);
          var sx = mapX(visStart, plotWidth);
          var sw = Math.max(3, ((visEnd - visStart) / rangeMs()) * plotWidth);
          var sy = Number(cfg.laneTop[laneForSpan] || 0) + Number(span.row || 0) * ((cfg.trackH || 12) + (cfg.trackGap || 1));
          var selected = selectedSpanId && selectedSpanId === span.id;
          var stroke = selected ? '#ef4444' : '#0f172a';
          var strokeWidth = selected ? 2 : 1;
          out.push('<g class="prof-span" data-span-id="' + escXml(span.id) + '" style="cursor:pointer">');
          out.push('<rect x="' + sx + '" y="' + sy + '" width="' + sw + '" height="' + (cfg.trackH || 12) + '" rx="4" fill="' + escXml(span.color || '#1d4ed8') + '" stroke="' + stroke + '" stroke-width="' + strokeWidth + '" />');
          if (sw >= 48) {
            out.push('<text x="' + (sx + 4) + '" y="' + (sy + 9) + '" fill="#f8fafc" font-size="9" font-weight="600" style="pointer-events:none">' + escXml(shortLabel(span.label || '(span)', 24)) + '</text>');
          }
          out.push('<title>' + escXml((span.label || '(span)') + ' | lane=' + span.lane + ' | ' + fmt(span.startMs) + '-' + fmt(span.endMs) + 'ms | ' + fmt(span.durationMs) + 'ms') + '</title>');
          out.push('</g>');
        }

        svg.innerHTML = out.join('');
        if (totalEl) totalEl.textContent = fmt(maxMs) + 'ms';
        if (windowEl) windowEl.textContent = fmt(viewMinMs) + 'ms - ' + fmt(viewMaxMs) + 'ms';
        if (zoomEl) zoomEl.textContent = (maxMs / rangeMs()).toFixed(2) + 'x';
      }

      function zoomAt(clientX, deltaY) {
        if (!canvas) return;
        var rect = canvas.getBoundingClientRect();
        var laneLeft = rect.left + cfg.leftPad;
        var laneRight = rect.left + Math.max(cfg.leftPad + 1, rect.width - cfg.rightPad);
        var ratio = (clientX - laneLeft) / Math.max(1, laneRight - laneLeft);
        ratio = clamp(ratio, 0, 1);
        var oldRange = rangeMs();
        var anchorMs = viewMinMs + ratio * oldRange;
        var factor = Math.exp(Number(deltaY || 0) * 0.0015);
        var newRange = clamp(oldRange * factor, minWindowMs, maxMs);
        if (newRange >= maxMs - 0.000001) {
          viewMinMs = 0;
          viewMaxMs = maxMs;
          render();
          return;
        }
        var nextMin = anchorMs - ratio * newRange;
        var nextMax = nextMin + newRange;
        if (nextMin < 0) {
          nextMax -= nextMin;
          nextMin = 0;
        }
        if (nextMax > maxMs) {
          var overflow = nextMax - maxMs;
          nextMin = Math.max(0, nextMin - overflow);
          nextMax = maxMs;
        }
        viewMinMs = clamp(nextMin, 0, maxMs - minWindowMs);
        viewMaxMs = clamp(nextMax, viewMinMs + minWindowMs, maxMs);
        render();
      }

      if (svg) {
        svg.addEventListener('click', function (event) {
          var node = event.target && event.target.closest ? event.target.closest('[data-span-id]') : null;
          if (!node) return;
          var id = node.getAttribute('data-span-id');
          if (!id || !map[id]) return;
          selectedSpanId = id;
          show(id);
          render();
        });
      }

      if (canvas) {
        canvas.addEventListener('wheel', function (event) {
          event.preventDefault();
          zoomAt(event.clientX, event.deltaY);
        }, { passive: false });
      }

      if (resetBtn) {
        resetBtn.addEventListener('click', function () {
          viewMinMs = 0;
          viewMaxMs = maxMs;
          render();
        });
      }

      window.addEventListener('resize', render);
      if (selectedSpanId) {
        show(selectedSpanId);
      }
      render();
    })();
  </script>
</body>
</html>`;
}

function renderLinkingDiagramPage({ pageName, graph, errorMessage = null }) {
  const nodes = Array.isArray(graph?.nodes) ? graph.nodes.slice() : [];
  const edges = Array.isArray(graph?.edges) ? graph.edges.slice() : [];
  const rootId = graph?.rootId ? String(graph.rootId) : `pages/${pageName}`;

  const outgoing = new Map();
  for (const edge of edges) {
    if (!outgoing.has(edge.from)) outgoing.set(edge.from, []);
    outgoing.get(edge.from).push(edge.to);
  }

  const depth = new Map();
  depth.set(rootId, 0);
  const queue = [rootId];
  while (queue.length > 0) {
    const cur = queue.shift();
    const curDepth = depth.get(cur) ?? 0;
    const nextList = outgoing.get(cur) || [];
    for (const next of nextList) {
      if (!depth.has(next) || (depth.get(next) > curDepth + 1)) {
        depth.set(next, curDepth + 1);
        queue.push(next);
      }
    }
  }

  let maxDepth = 0;
  for (const value of depth.values()) {
    maxDepth = Math.max(maxDepth, value);
  }
  for (const node of nodes) {
    if (!depth.has(node.id)) {
      maxDepth += 1;
      depth.set(node.id, maxDepth);
    }
  }

  const groups = new Map();
  for (const node of nodes) {
    const d = depth.get(node.id) ?? 0;
    if (!groups.has(d)) groups.set(d, []);
    groups.get(d).push(node);
  }
  for (const group of groups.values()) {
    group.sort((a, b) => String(a.path).localeCompare(String(b.path)));
  }

  const boxW = 320;
  const boxH = 144;
  const xGap = 180;
  const yGap = 34;
  const pad = 40;

  let maxRows = 1;
  for (const group of groups.values()) maxRows = Math.max(maxRows, group.length);
  const svgWidth = pad * 2 + (maxDepth + 1) * boxW + maxDepth * xGap;
  const svgHeight = pad * 2 + maxRows * boxH + Math.max(0, maxRows - 1) * yGap;

  const positions = new Map();
  for (let d = 0; d <= maxDepth; d++) {
    const group = groups.get(d) || [];
    const totalHeight = group.length * boxH + Math.max(0, group.length - 1) * yGap;
    let y = Math.max(pad, (svgHeight - totalHeight) / 2);
    const x = pad + d * (boxW + xGap);
    for (const node of group) {
      positions.set(node.id, { x, y, w: boxW, h: boxH });
      y += boxH + yGap;
    }
  }

  const esc = escapeHtml;
  const edgeColor = (kind) => {
    if (kind === 'linked_vm') return '#16a34a';
    if (kind === 'linked_edge') return '#059669';
    if (kind === 'dynamic_vm') return '#d97706';
    if (kind === 'static_origin') return '#2563eb';
    if (kind === 'data_origin_first') return '#0f766e';
    if (kind === 'data_edge_delayed') return '#c2410c';
    return '#64748b';
  };

  const edgeSvg = edges
    .filter((edge) => positions.has(edge.from) && positions.has(edge.to))
    .map((edge) => {
      const from = positions.get(edge.from);
      const to = positions.get(edge.to);
      const x1 = from.x + from.w;
      const y1 = from.y + from.h / 2;
      const x2 = to.x;
      const y2 = to.y + to.h / 2;
      const cx = Math.max(60, (x2 - x1) * 0.5);
      const stroke = edgeColor(edge.callKind);
      const markerKind = (edge.callKind === 'static_origin' ||
        edge.callKind === 'dynamic_vm' ||
        edge.callKind === 'linked_vm' ||
        edge.callKind === 'linked_edge' ||
        edge.callKind === 'data_origin_first' ||
        edge.callKind === 'data_edge_delayed' ||
        edge.callKind === 'page')
        ? edge.callKind
        : 'unknown';
      let label = '';
      if (edge.edgeType === 'data') {
        const qText = Array.isArray(edge.queryRefs) && edge.queryRefs.length > 0
          ? edge.queryRefs.map((value) => `q${value}`).join(',')
          : 'q?';
        const modeText = Array.isArray(edge.fetchModes) && edge.fetchModes.length > 0
          ? edge.fetchModes.map(dataFetchModeLabel).join(' | ')
          : linkingKindLabel(edge.callKind);
        label = `${modeText} | ${qText} | calls=${edge.count} | rows=${edge.rowCount} | total=${formatMs(edge.totalMs)}ms`;
      } else {
        const opcodes = Array.isArray(edge.componentOpcodes) && edge.componentOpcodes.length > 0
          ? edge.componentOpcodes.join(',')
          : 'n/a';
        label = `${linkingKindLabel(edge.callKind)} | op=${opcodes} | calls=${edge.count} | total=${formatMs(edge.totalMs)}ms`;
      }
      return `<g>
  <path d="M ${x1} ${y1} C ${x1 + cx} ${y1}, ${x2 - cx} ${y2}, ${x2} ${y2}" stroke="${stroke}" stroke-width="2" fill="none" marker-end="url(#arrow-${markerKind})" />
  <text x="${(x1 + x2) / 2}" y="${(y1 + y2) / 2 - 6}" text-anchor="middle" font-size="11" fill="${stroke}">${esc(label)}</text>
</g>`;
    })
    .join('\n');

  const nodeSvg = nodes
    .filter((node) => positions.has(node.id))
    .map((node) => {
      const pos = positions.get(node.id);
      const isDataNode = node.nodeType === 'data_fetch';
      if (isDataNode) {
        const fill = dataFetchModeColor(node.fetchModes);
        const modeText = Array.isArray(node.fetchModes) && node.fetchModes.length > 0
          ? node.fetchModes.map(dataFetchModeLabel).join(' | ')
          : 'unknown';
        const qText = Array.isArray(node.queryRefs) && node.queryRefs.length > 0
          ? node.queryRefs.map((value) => `q${value}`).join(', ')
          : 'q?';
        const nameText = Array.isArray(node.queryNames) && node.queryNames.length > 0
          ? node.queryNames.join(', ')
          : 'unknown';
        const line1 = `Data ${node.label || qText}`;
        const line2 = `Stage: ${modeText}`;
        const line3 = `Query: ${qText} | Name: ${nameText}`;
        const line4 = `Calls: ${node.fetchCount} | Rows: ${node.rowCount}`;
        const line5 = `Self: ${formatMs(node.selfMs)}ms | Total: ${formatMs(node.totalMs)}ms`;
        const line6 = `Status: ${node.errorCount > 0 ? `errors=${node.errorCount}` : 'ok'}`;
        const points = [
          `${pos.x + 18},${pos.y}`,
          `${pos.x + pos.w - 18},${pos.y}`,
          `${pos.x + pos.w},${pos.y + pos.h / 2}`,
          `${pos.x + pos.w - 18},${pos.y + pos.h}`,
          `${pos.x + 18},${pos.y + pos.h}`,
          `${pos.x},${pos.y + pos.h / 2}`,
        ].join(' ');
        return `<g>
  <polygon points="${points}" fill="${fill}" stroke="#0f172a" stroke-width="1.5" />
  <text x="${pos.x + 12}" y="${pos.y + 24}" fill="#f8fafc" font-size="13" font-weight="700">${esc(line1)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 46}" fill="#e2e8f0" font-size="11">${esc(line2)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 66}" fill="#cbd5e1" font-size="11">${esc(line3)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 86}" fill="#cbd5e1" font-size="11">${esc(line4)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 106}" fill="#cbd5e1" font-size="11">${esc(line5)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 122}" fill="#cbd5e1" font-size="11">${esc(line6)}</text>
</g>`;
      }

      const fill = linkingNodeColor(node);
      const kindText = node.callKinds.length > 0
        ? node.callKinds.map(linkingKindLabel).join(' | ')
        : 'Unknown';
      const line1 = node.path;
      const line2 = `Kind: ${kindText}`;
      const line3 = `Size: ${formatBytes(node.sizeBytes)} | Renders: ${node.renderCount}`;
      const line4 = `Self: ${formatMs(node.selfMs)}ms | Total: ${formatMs(node.totalMs)}ms`;
      const line5 = `Engine: ${(node.renderEngines || []).join(', ') || 'unknown'} | Load: ${(node.loadSources || []).join(', ') || 'unknown'}`;
      const line6 = `Opcode: ${(node.componentOpcodes || []).join(', ') || 'n/a'}`;
      return `<g>
  <rect x="${pos.x}" y="${pos.y}" width="${pos.w}" height="${pos.h}" rx="10" fill="${fill}" stroke="#0f172a" stroke-width="1.5" />
  <text x="${pos.x + 12}" y="${pos.y + 24}" fill="#f8fafc" font-size="13" font-weight="700">${esc(line1)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 46}" fill="#e2e8f0" font-size="11">${esc(line2)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 66}" fill="#cbd5e1" font-size="11">${esc(line3)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 86}" fill="#cbd5e1" font-size="11">${esc(line4)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 106}" fill="#cbd5e1" font-size="11">${esc(line5)}</text>
  <text x="${pos.x + 12}" y="${pos.y + 122}" fill="#cbd5e1" font-size="11">${esc(line6)}</text>
</g>`;
    })
    .join('\n');

  const errorBanner = errorMessage
    ? `<div class="error">Execution completed with error: ${esc(errorMessage)}</div>`
    : '';

  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Mot Linking Diagram - ${esc(pageName)}</title>
  <style>
    body { margin: 0; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; background: #f8fafc; color: #0f172a; }
    .wrap { padding: 18px 20px 26px; }
    h1 { margin: 0 0 6px; font-size: 22px; }
    p { margin: 0 0 10px; color: #334155; }
    .legend { display: flex; gap: 14px; flex-wrap: wrap; margin: 8px 0 14px; font-size: 13px; }
    .legend span::before { content: ''; display: inline-block; width: 10px; height: 10px; border-radius: 999px; margin-right: 6px; vertical-align: baseline; }
    .k-static::before { background: #2563eb; }
    .k-dynamic::before { background: #d97706; }
    .k-linked-vm::before { background: #16a34a; }
    .k-linked::before { background: #059669; }
    .k-page::before { background: #334155; }
    .k-data-origin::before { background: #0f766e; border-radius: 2px; transform: rotate(45deg); }
    .k-data-delayed::before { background: #c2410c; border-radius: 2px; transform: rotate(45deg); }
    .error { margin: 8px 0 14px; padding: 10px 12px; border: 1px solid #dc2626; background: #fef2f2; color: #991b1b; border-radius: 8px; }
    .canvas { border: 1px solid #cbd5e1; background: #ffffff; border-radius: 10px; overflow: auto; box-shadow: 0 8px 20px rgba(15, 23, 42, 0.08); }
    svg { display: block; }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Component Linking Diagram</h1>
    <p>Page: <b>${esc(pageName)}</b>. Component boxes show size, self time, and total time. Data fetches are hexagons. Arrows show call/link/fetch type.</p>
    <p><a href="${esc(pageModeHref(pageName, 'linking-flow'))}">Open Artifact + Dataflow Timeline View</a> | <a href="${esc(pageModeHref(pageName, 'linking-profiler'))}">Open Rendering Profiler View</a></p>
    ${errorBanner}
    <div class="legend">
      <span class="k-page">Page root</span>
      <span class="k-static">Static (origin-compiled)</span>
      <span class="k-dynamic">Dynamic load (edge VM)</span>
      <span class="k-linked-vm">Linked opcode (edge VM)</span>
      <span class="k-linked">Linked native (edge)</span>
      <span class="k-data-origin">Data fetch (origin first render, hexagon)</span>
      <span class="k-data-delayed">Data fetch (edge delayed, hexagon)</span>
    </div>
    <div class="canvas">
      <svg width="${svgWidth}" height="${svgHeight}" viewBox="0 0 ${svgWidth} ${svgHeight}" xmlns="http://www.w3.org/2000/svg">
        <defs>
          <marker id="arrow-static_origin" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#2563eb"/></marker>
          <marker id="arrow-dynamic_vm" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#d97706"/></marker>
          <marker id="arrow-linked_vm" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#16a34a"/></marker>
          <marker id="arrow-linked_edge" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#059669"/></marker>
          <marker id="arrow-data_origin_first" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#0f766e"/></marker>
          <marker id="arrow-data_edge_delayed" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#c2410c"/></marker>
          <marker id="arrow-page" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#64748b"/></marker>
          <marker id="arrow-unknown" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="#64748b"/></marker>
        </defs>
        ${edgeSvg}
        ${nodeSvg}
      </svg>
    </div>
  </div>
</body>
</html>`;
}

async function handleLinkingDiagramRequest({
  request,
  originUrl,
  vmMode,
  pageName,
  viewMode = 'graph',
}) {
  const trace = new RenderTrace({ page: pageName, vmMode, originUrl, debugMode: 'linking' });
  const fetchSpan = trace.startSpan('page_bytecode_fetch', { page: pageName });
  const fetched = await fetchPageBytecode(originUrl, pageName);
  trace.endSpan(fetchSpan, fetched.ok ? 'ok' : 'error', { status: fetched.status ?? 500 });

  if (!fetched.ok) {
    return new Response(`<!DOCTYPE html><html><body><h1>Page Not Found</h1><p>${escapeHtml(pageName)}</p></body></html>`, {
      status: 404,
      headers: { 'Content-Type': 'text/html; charset=utf-8' },
    });
  }

  const bytecode = fetched.bytecode;
  const totalBytes = fetched.totalBytes;
  const flags = getBytecodeFlags(bytecode);
  const authRequired = (flags & BYTECODE_FLAG_AUTH_REQUIRED) !== 0;
  const adminRequired = (flags & BYTECODE_FLAG_ADMIN_REQUIRED) !== 0;

  if (authRequired) {
    const cookies = parseCookies(request);
    const sessionToken = cookies.session;
    if (!sessionToken) {
      return new Response('<!DOCTYPE html><html><head><meta http-equiv="refresh" content="0;url=/login"></head><body>Redirecting to login...</body></html>', {
        headers: { 'Content-Type': 'text/html; charset=utf-8' },
      });
    }
    const sessionData = await validateSession(originUrl, sessionToken);
    if (!sessionData.valid) {
      return new Response('<!DOCTYPE html><html><head><meta http-equiv="refresh" content="0;url=/login"></head><body>Session expired. Redirecting to login...</body></html>', {
        headers: { 'Content-Type': 'text/html; charset=utf-8' },
      });
    }
    if (adminRequired && sessionData.user?.role !== 'admin') {
      return new Response('<!DOCTYPE html><html><body><h1>403 Forbidden</h1><p>Admin access required.</p></body></html>', {
        status: 403,
        headers: { 'Content-Type': 'text/html; charset=utf-8' },
      });
    }
  }

  const debugParseSpan = trace.startSpan('bytecode_debug_extract');
  const pageDebugMeta = extractBytecodeDebugMetadata(bytecode);
  pageDebugMeta.sourceMap = buildBytecodeDebugSourceMap(pageDebugMeta, {
    file: `${pageName}.wasm`,
    source: `pages/${pageName}.mot`,
  });
  const pageComponentDebugMetaByFunction = buildWasmComponentDebugMetaByFunction(
    pageDebugMeta,
    {
      componentPath: `pages/${pageName}`,
      componentName: pageName,
      sourcePath: `pages/${pageName}.mot`,
      loadSource: 'static',
      artifactSource: 'inline',
    },
  );
  trace.setDebugMetadata(pageDebugMeta);
  trace.endSpan(debugParseSpan, 'ok');

  const dependencies = [];
  const tracker = new LinkingGraphTracker({ pageName, pageSizeBytes: totalBytes });
  const profiler = new LinkingProfilerTracker({ pageName });
  tracker.beginRoot();
  profiler.begin();

  const writer = { write: async () => {} };
  const encoder = new TextEncoder();
  const renderStartedAt = nowMs();
  let runError = null;
  try {
    const wasmResult = await runWithWasmInterpreter({
      bytecode,
      writer,
      encoder,
      originUrl,
      pageName,
      dependencies,
      trace,
      debugEnabled: true,
      componentDebugMetaByFunction: pageComponentDebugMetaByFunction,
      onComponentRender: (event) => {
        tracker.onComponentEvent(event);
        profiler.onComponentEvent(event);
      },
      onDataFetch: (event) => {
        tracker.onDataFetch(event);
        profiler.onDataFetch(event);
      },
      onVmAwait: (event) => profiler.onVmAwait(event),
      onVmResume: (event) => profiler.onVmResume(event),
    });
    if (!wasmResult.success) {
      throw new Error(wasmResult.fallbackReason || 'WASM VM failed');
    }
    trace.setResult({ vmUsed: wasmResult.vmUsed, bytecodeSize: totalBytes, dependencies });
  } catch (error) {
    runError = error;
    trace.setError(error?.message || String(error));
  } finally {
    tracker.endRoot(nowMs() - renderStartedAt, runError ? 'error' : 'ok');
    profiler.end(runError ? 'error' : 'ok', runError ? (runError.message || String(runError)) : null);
  }

  const graph = tracker.toGraph();
  const profile = profiler.toJSON();
  const errorMessage = runError ? (runError.message || String(runError)) : null;
  let html = '';
  if (viewMode === 'flow') {
    html = renderLinkingFlowPage({
      pageName,
      graph,
      trace,
      errorMessage,
    });
  } else if (viewMode === 'profiler') {
    html = renderLinkingProfilerPage({
      pageName,
      profile,
      errorMessage,
    });
  } else {
    html = renderLinkingDiagramPage({
      pageName,
      graph,
      errorMessage,
    });
  }
  return new Response(html, {
    status: runError ? 500 : 200,
    headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' },
  });
}

/**
 * Main request handler
 */
async function handleRequest(request, env) {
  const url = new URL(request.url);
  const path = url.pathname;
  const motMode = String(url.searchParams.get('mot') || '').trim().toLowerCase();
  const linkingMode = motMode === 'linking';
  const linkingFlowMode = motMode === 'linking-flow';
  const linkingProfilerMode = motMode === 'linking-profiler';
  const stepMode = motMode === 'step';
  const stepLiveMode = motMode === 'step-live';
  const originPort = Number(env.ORIGIN_PORT || 8080);
  const originUrl = env.ORIGIN_URL || `http://localhost:${originPort}`;
  const vmMode = resolveVmMode(env.MOT_VM);
  const devMode = isDevMode(env);
  const hotMode = isHotMode(env);
  const dsdEnabled = isDsdEnabled(env);
  const browserWasmEnabled = shouldEnableBrowserWasm(env, url) || stepLiveMode;
  const debugEnabled = isDebugEnabled(env) || linkingMode || linkingFlowMode || linkingProfilerMode || stepMode || stepLiveMode;

  // In dev/hot mode, clear component caches so we always fetch fresh from origin
  if (devMode || hotMode) {
    componentBytecodeCache.clear();
    componentBytecodeMetaCache.clear();
    bundledComponentBytecodeCache.clear();
    debugSourceCache.clear();
  }

  console.log(`[edge] Request: ${path} (vm=${vmMode}${devMode ? ' dev' : ''})`);

  // Health check
  if (path === '/health') {
    return new Response(JSON.stringify({ status: 'ok', runtime: 'edge', vm: vmMode, originUrl }), {
      headers: { 'Content-Type': 'application/json' },
    });
  }

  // Dev/hot-mode SSE proxy: stream change events from origin to browser
  if ((devMode || hotMode) && path === `${BROWSER_RPC_BASE_PATH}/dev/events`) {
    const proxy = await fetch(`${originUrl}/_dev/events`, {
      method: 'GET',
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache',
        'Access-Control-Allow-Origin': '*',
        'Connection': 'keep-alive',
      },
    });
  }

  if (path === `${BROWSER_RPC_BASE_PATH}/runtime/wasm`) {
    const proxy = await fetch(`${originUrl}/runtime/wasm`, {
      method: 'GET',
      headers: {
        'Cookie': request.headers.get('Cookie') || '',
      },
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: proxy.headers,
    });
  }

  if (path.startsWith(`${BROWSER_RPC_BASE_PATH}/page/`)) {
    const pagePath = path.slice(`${BROWSER_RPC_BASE_PATH}/page/`.length);
    const proxy = await fetch(`${originUrl}/page/${pagePath}`, {
      method: 'GET',
      headers: {
        'Cookie': request.headers.get('Cookie') || '',
      },
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: proxy.headers,
    });
  }

  if (path.startsWith(`${BROWSER_RPC_BASE_PATH}/component/`)) {
    const componentPath = path.slice(`${BROWSER_RPC_BASE_PATH}/component/`.length);
    const proxy = await fetch(`${originUrl}/component/${componentPath}`, {
      method: 'GET',
      headers: {
        'Cookie': request.headers.get('Cookie') || '',
      },
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: proxy.headers,
    });
  }

  if (path.startsWith(`${BROWSER_RPC_BASE_PATH}/source/`)) {
    const sourcePath = path.slice(`${BROWSER_RPC_BASE_PATH}/source/`.length);
    const proxy = await fetch(`${originUrl}/source/${sourcePath}`, {
      method: 'GET',
      headers: {
        'Cookie': request.headers.get('Cookie') || '',
      },
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: proxy.headers,
    });
  }

  if (path.startsWith(`${BROWSER_RPC_BASE_PATH}/data/`) && request.method === 'POST') {
    const dataPath = path.slice(`${BROWSER_RPC_BASE_PATH}/data/`.length);
    const proxy = await fetch(`${originUrl}/data/${dataPath}`, {
      method: 'POST',
      headers: {
        'Content-Type': request.headers.get('Content-Type') || 'application/json',
        'Cookie': request.headers.get('Cookie') || '',
      },
      body: await request.text(),
    });
    return new Response(proxy.body, {
      status: proxy.status,
      headers: proxy.headers,
    });
  }

  if (path === `${BROWSER_RPC_BASE_PATH}/playground/render` && request.method === 'POST') {
    return renderPlaygroundSourceWithWasm({ request, originUrl, debugEnabled });
  }

  // Forward auth endpoints to origin
  if (path === '/login' || path === '/logout' || path === '/session') {
    const originResponse = await fetch(`${originUrl}${path}`, {
      method: request.method,
      headers: {
        'Content-Type': request.headers.get('Content-Type') || 'application/json',
        'Cookie': request.headers.get('Cookie') || '',
      },
      body: request.method !== 'GET' ? await request.text() : undefined,
    });

    // Forward response including Set-Cookie headers
    const headers = new Headers();
    originResponse.headers.forEach((value, key) => {
      headers.append(key, value);
    });

    return new Response(await originResponse.text(), {
      status: originResponse.status,
      headers,
    });
  }

  // Map URL path to page name
  let pageName = path === '/' ? 'index' : path.slice(1);

  // Remove trailing slash
  if (pageName.endsWith('/')) {
    pageName = pageName.slice(0, -1);
  }

  if (linkingMode || linkingFlowMode || linkingProfilerMode) {
    return handleLinkingDiagramRequest({
      request,
      originUrl,
      vmMode,
      pageName,
      viewMode: linkingProfilerMode ? 'profiler' : (linkingFlowMode ? 'flow' : 'graph'),
    });
  }

  // Create streaming response
  const { readable, writable } = new TransformStream();
  const writer = writable.getWriter();
  const encoder = new TextEncoder();

  // Track dependencies for incremental updates
  const dependencies = [];
  const trace = debugEnabled ? new RenderTrace({ page: pageName, vmMode, originUrl, debugMode: motMode }) : null;
  const vmStepDebugState = stepMode ? createVmStepDebugState(pageName) : null;
  let pageDebugMeta = null;
  let pageComponentDebugMetaByFunction = null;

  // Start streaming in background
  (async () => {
    try {
      // Fetch page bytecode from origin
      console.log(`[edge] Fetching page: ${pageName}`);
      const fetchSpan = trace ? trace.startSpan('page_bytecode_fetch', { page: pageName }) : null;
      const fetched = await fetchPageBytecode(originUrl, pageName);
      if (trace && fetchSpan) {
        trace.endSpan(fetchSpan, fetched.ok ? 'ok' : 'error', { status: fetched.status ?? 500 });
      }

      if (!fetched.ok) {
        await writer.write(encoder.encode(`<!DOCTYPE html>
<html>
<head><title>Error</title></head>
<body>
  <h1>Page Not Found</h1>
  <p>The page "${escapeHtml(pageName)}" could not be found.</p>
</body>
</html>`));
        await writer.close();
        return;
      }

      const bytecode = fetched.bytecode;
      const totalBytes = fetched.totalBytes;
      const reactiveWasmPayload = browserWasmEnabled
        ? buildReactiveWasmPayload(bytecode)
        : null;

      console.log(`[edge] Bytecode received: ${totalBytes} bytes`);
      // DSD needs the func_idx → componentPath map that lives inside
      // bytecode debug metadata, so extract it whenever DSD is enabled
      // even if tracing/debug is off.
      const needDebugMeta = Boolean(trace) || dsdEnabled;
      if (needDebugMeta) {
        const debugParseSpan = trace ? trace.startSpan('bytecode_debug_extract') : null;
        pageDebugMeta = extractBytecodeDebugMetadata(bytecode);
        pageDebugMeta.sourceMap = buildBytecodeDebugSourceMap(pageDebugMeta, {
          file: `${pageName}.wasm`,
          source: `pages/${pageName}.mot`,
        });
        pageComponentDebugMetaByFunction = buildWasmComponentDebugMetaByFunction(
          pageDebugMeta,
          {
            componentPath: `pages/${pageName}`,
            componentName: pageName,
            sourcePath: `pages/${pageName}.mot`,
            loadSource: 'static',
            artifactSource: 'inline',
          },
        );
        if (trace) {
          trace.setDebugMetadata(pageDebugMeta);
          trace.endSpan(debugParseSpan, 'ok');
        }
      }
      // Check auth flags in bytecode
      const flags = getBytecodeFlags(bytecode);
      const authRequired = (flags & BYTECODE_FLAG_AUTH_REQUIRED) !== 0;
      const adminRequired = (flags & BYTECODE_FLAG_ADMIN_REQUIRED) !== 0;

      if (authRequired) {
        console.log(`[edge] Page requires auth (admin=${adminRequired})`);
        const cookies = parseCookies(request);
        const sessionToken = cookies['session'];

        if (!sessionToken) {
          // No session - redirect to login
          console.log('[edge] No session cookie, redirecting to login');
          await writer.write(encoder.encode(`<!DOCTYPE html>
<html>
<head><meta http-equiv="refresh" content="0;url=/login"></head>
<body>Redirecting to login...</body>
</html>`));
          await writer.close();
          return;
        }

        // Validate session with origin
        const sessionData = await validateSession(originUrl, sessionToken);
        if (!sessionData.valid) {
          console.log('[edge] Invalid session, redirecting to login');
          await writer.write(encoder.encode(`<!DOCTYPE html>
<html>
<head><meta http-equiv="refresh" content="0;url=/login"></head>
<body>Session expired. Redirecting to login...</body>
</html>`));
          await writer.close();
          return;
        }

        // Check admin role if required
        if (adminRequired && sessionData.user?.role !== 'admin') {
          console.log('[edge] Admin required but user is not admin');
          await writer.write(encoder.encode(`<!DOCTYPE html>
<html>
<head><title>Forbidden</title></head>
<body>
  <h1>403 Forbidden</h1>
  <p>Admin access required.</p>
  <p><a href="/login">Login with different account</a></p>
</body>
</html>`));
          await writer.close();
          return;
        }

        console.log(`[edge] Authenticated user: ${sessionData.user?.username}`);
      }

      if (stepLiveMode) {
        if (trace) {
          trace.recordExecutionPath('vm:browser-wasm-live');
          trace.setResult({ vmUsed: 'browser-wasm-live', bytecodeSize: totalBytes, dependencies });
        }

        const liveHeader = `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Mot Live Step Debug - ${escapeHtml(pageName)}</title>
  <style>
    body { margin: 0; font-family: ui-monospace, Menlo, Monaco, Consolas, monospace; background: #020617; color: #e2e8f0; }
    #mot-live-step-banner { margin: 12px; padding: 10px 12px; border-radius: 8px; border: 1px solid #334155; background: #0f172a; }
    #mot-live-step-banner code { color: #93c5fd; }
  </style>
</head>
<body>
  <div id="mot-live-step-banner">
    Live step mode is running in-browser. Use the floating Mot Live Debug panel to step through source and inspect locals.
    Query: <code>?mot=step-live</code>
  </div>
`;
        await writer.write(encoder.encode(liveHeader));
        await writer.write(encoder.encode(`\n<!-- Motus Edge Runtime | vm: wasm | mode: browser-step-live | ${totalBytes} bytes -->\n`));
        if (trace) {
          await writer.write(encoder.encode(`${renderTraceScriptTag(trace)}\n`));
        }
        await writer.write(
          encoder.encode(
            `${renderBrowserWasmBootstrapScript({
              rpcBasePath: BROWSER_RPC_BASE_PATH,
              pageName,
              reactiveWasmPayload,
              initialPageBytecodeBase64: encodeBytesToBase64(bytecode),
              stepLiveMode: true,
              pauseOnDebugStep: false,
            })}\n`,
          ),
        );
        await writer.write(encoder.encode(`</body>\n</html>\n`));
        await writer.close();
        console.log('[edge] Response complete (browser step-live mode)');
        return;
      }

      let vmUsed = 'wasm';
      const wasmSpan = trace ? trace.startSpan('vm_wasm') : null;
      const wasmResult = await runWithWasmInterpreter({
        bytecode,
        writer,
        encoder,
        originUrl,
        pageName,
        dependencies,
        trace,
        debugEnabled,
        componentDebugMetaByFunction: pageComponentDebugMetaByFunction,
        onDebugStep: vmStepDebugState
          ? (event) => {
            recordVmStepDebugEvent(vmStepDebugState, event);
          }
          : null,
        enableDSD: dsdEnabled,
      });
      if (trace && wasmSpan) {
        trace.endSpan(wasmSpan, wasmResult.success ? 'ok' : 'error', {
          reason: wasmResult.fallbackReason || null,
        });
      }
      if (!wasmResult.success) {
        throw new Error(wasmResult.fallbackReason || 'WASM VM failed');
      }
      vmUsed = wasmResult.vmUsed;

      // Add debug comment at end
      await writer.write(encoder.encode(`\n<!-- Motus Edge Runtime | vm: ${vmUsed} | ${totalBytes} bytes | deps: ${dependencies.join(', ') || 'none'} -->\n`));
      if (trace) {
        trace.setResult({ vmUsed, bytecodeSize: totalBytes, dependencies });
        if (vmStepDebugState) {
          const vmStepDebug = await finalizeVmStepDebug(vmStepDebugState, originUrl);
          trace.setVmStepDebug(vmStepDebug);
        }
        await writer.write(encoder.encode(`${renderTraceScriptTag(trace)}\n`));
        await writer.write(encoder.encode(`${renderDevtoolsBootstrapScript()}\n`));
      }
      if (browserWasmEnabled) {
        await writer.write(
          encoder.encode(
            `${renderBrowserWasmBootstrapScript({ rpcBasePath: BROWSER_RPC_BASE_PATH, pageName, reactiveWasmPayload })}\n`,
          ),
        );
      }
      if (hotMode) {
        await writer.write(encoder.encode(`${renderHotReloadScript()}\n`));
      } else if (devMode) {
        await writer.write(encoder.encode(`${renderLiveReloadScript()}\n`));
      }

      await writer.close();
      console.log('[edge] Response complete');

    } catch (error) {
      console.error('[edge] Error:', error);
      if (trace) {
        trace.setError(error?.message || String(error));
      }
      await writer.write(encoder.encode(`<!DOCTYPE html>
<html>
<head><title>Error</title></head>
<body>
  <h1>Error</h1>
  <p>${escapeHtml(error.message)}</p>
  <pre>${escapeHtml(error.stack || '')}</pre>
</body>
</html>`));
      if (trace) {
        if (vmStepDebugState) {
          const vmStepDebug = await finalizeVmStepDebug(vmStepDebugState, originUrl);
          trace.setVmStepDebug(vmStepDebug);
        }
        await writer.write(encoder.encode(`${renderTraceScriptTag(trace)}\n`));
        await writer.write(encoder.encode(`${renderDevtoolsBootstrapScript()}\n`));
      }
      if (browserWasmEnabled) {
        await writer.write(
          encoder.encode(
            `${renderBrowserWasmBootstrapScript({ rpcBasePath: BROWSER_RPC_BASE_PATH, pageName, reactiveWasmPayload: null })}\n`,
          ),
        );
      }
      await writer.close();
    }
  })();

  return createStreamingResponse(readable);
}

/**
 * Escape HTML special characters
 */
function escapeHtml(str) {
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

/**
 * Export worker
 */
export default {
  async fetch(request, env, ctx) {
    return handleRequest(request, env);
  },
};
