function isTruthyFlag(value) {
  const normalized = String(value ?? '').trim().toLowerCase();
  return normalized === '1' || normalized === 'true' || normalized === 'on' || normalized === 'yes';
}

export function shouldEnableBrowserWasm(env, url) {
  const envEnabled = isTruthyFlag(env?.MOT_BROWSER_WASM);
  const queryRaw = url?.searchParams?.get('browser_wasm');
  if (queryRaw != null) {
    return isTruthyFlag(queryRaw);
  }
  return envEnabled;
}

export function renderBrowserWasmBootstrapScript(options = {}) {
  const rpcBasePath = JSON.stringify(String(options.rpcBasePath || '/_mot'));
  const pageName = JSON.stringify(String(options.pageName || ''));
  const reactiveWasmPayload = JSON.stringify(options.reactiveWasmPayload || null);
  const initialPageBytecodeBase64 = JSON.stringify(String(options.initialPageBytecodeBase64 || ''));
  const stepLiveMode = options.stepLiveMode === true;
  const pauseOnDebugStep = options.pauseOnDebugStep === true;
  const stepLiveModeJson = JSON.stringify(stepLiveMode);
  const pauseOnDebugStepJson = JSON.stringify(pauseOnDebugStep);

  return `<script id="mot-browser-wasm-bootstrap">
(function () {
  if (window.__motBrowserWasmBootstrapped) return;
  window.__motBrowserWasmBootstrapped = true;

  var rpcBasePath = ${rpcBasePath};
  var pageName = ${pageName};
  var reactiveWasmPayload = ${reactiveWasmPayload};
  var initialPageBytecodeBase64 = ${initialPageBytecodeBase64};
  var stepLiveMode = ${stepLiveModeJson};
  var pauseOnDebugStep = ${pauseOnDebugStepJson};
  if (!rpcBasePath || !pageName || typeof WebAssembly === 'undefined') {
    return;
  }
  window.__motLiveStepMode = stepLiveMode;

  var decoder = new TextDecoder();
  var encoder = new TextEncoder();
  var MOT_OK = 0;
  var MOT_AWAIT = -2;
  var runtimeWasmBytesPromise = null;
  var pageBytecodeCache = Object.create(null);
  var componentBytecodeCache = Object.create(null);
  var debugSourceLinesCache = Object.create(null);
  var debugPauseScriptCache = Object.create(null);
  var debugPauseScriptCacheSize = 0;
  var signalRegistry = Object.create(null);
  var varBindings = Object.create(null);
  var attrNodeIndex = Object.create(null);
  var exprNodeIndex = Object.create(null);
  var reactiveState = Object.create(null);
  var reactiveWasmExports = null;
  var reactiveWasmLoading = false;
  var mutationQueue = Object.create(null);
  var mutationQueueOrder = [];
  var mutationFlushScheduled = false;
  var reactiveMeta = {
    bindPaths: Object.create(null),
    setPaths: Object.create(null),
    bindAttrRules: [],
    planSetToBind: Object.create(null),
    planAttrBySet: Object.create(null),
    setToBind: Object.create(null),
    rootToBind: Object.create(null),
    setToAttr: Object.create(null),
    rootToAttr: Object.create(null),
    exprPrograms: Object.create(null),
    exprDepsById: Object.create(null),
    exprAttrRules: [],
    planExprBySet: Object.create(null),
    setToExpr: Object.create(null),
    rootToExpr: Object.create(null),
    setToExprAttr: Object.create(null),
    exprAttrByExprId: Object.create(null),
  };
  var OPCODE_NAMES = [
    'BC_NOP','BC_CONST','BC_POP','BC_DUP','BC_LOAD','BC_STORE','BC_LOAD_GLOBAL','BC_LOAD_FIELD',
    'BC_LOAD_INDEX','BC_STORE_FIELD','BC_STORE_INDEX','BC_NULL','BC_TRUE','BC_FALSE','BC_INT',
    'BC_ADD','BC_SUB','BC_MUL','BC_DIV','BC_MOD','BC_NEG','BC_EQ','BC_NEQ','BC_LT','BC_LTE',
    'BC_GT','BC_GTE','BC_AND','BC_OR','BC_NOT','BC_JUMP','BC_JUMP_IF_FALSE','BC_JUMP_IF_TRUE',
    'BC_ITER_START','BC_ITER_NEXT','BC_ITER_END','BC_EMIT_LITERAL','BC_EMIT_TEXT','BC_EMIT_RAW',
    'BC_EMIT_ATTR_START','BC_EMIT_ATTR_END','BC_EMIT_TAG_OPEN','BC_EMIT_TAG_END','BC_EMIT_TAG_CLOSE',
    'BC_EMIT_TAG_SELF','BC_FETCH_DATA','BC_FETCH_WAIT','BC_CALL','BC_CALL_BUILTIN','BC_CALL_PIPE',
    'BC_RETURN','BC_COMPONENT_START','BC_COMPONENT_END','BC_COMPONENT_LOAD','BC_SLOT_START','BC_SLOT_END',
    'BC_SLOT_DEFAULT','BC_DEP_START','BC_DEP_END','BC_ARRAY_NEW','BC_OBJECT_NEW','BC_OBJECT_SET',
    'BC_CONCAT','BC_HALT','BC_COMPONENT_LINKED',
  ];

  function normalizeU32(value) {
    return Number(value) >>> 0;
  }

  function rootOfPath(path) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return '';
    var dot = normalized.indexOf('.');
    return dot >= 0 ? normalized.slice(0, dot) : normalized;
  }

  function normalizeRpcBase(path) {
    var normalized = String(path || '').trim();
    if (!normalized) return '/_mot';
    if (normalized.charAt(0) !== '/') normalized = '/' + normalized;
    return normalized.replace(/\\/+$/, '');
  }

  function buildRuntimeWasmRpcUrl() {
    return normalizeRpcBase(rpcBasePath) + '/runtime/wasm';
  }

  function buildPageRpcUrl(page) {
    return normalizeRpcBase(rpcBasePath) + '/page/' + encodeURIComponent(page);
  }

  function normalizeComponentPath(path) {
    var raw = String(path || '').replace(/\\\\/g, '/').replace(/^\\/+|\\/+$/g, '');
    if (!raw) return '';
    while (raw.indexOf('./') === 0) raw = raw.slice(2);
    if (raw.indexOf('components/') !== 0) raw = 'components/' + raw;
    if (raw.indexOf('..') >= 0) return '';
    return raw;
  }

  function componentRoutePath(path) {
    var normalized = normalizeComponentPath(path);
    if (!normalized) return '';
    if (normalized.indexOf('components/') === 0) return normalized.slice('components/'.length);
    return normalized;
  }

  function buildComponentRpcUrl(componentPath) {
    return normalizeRpcBase(rpcBasePath) + '/component/' + encodeURIComponent(componentRoutePath(componentPath));
  }

  function normalizeSourcePath(sourcePath) {
    var src = String(sourcePath || '').trim().replace(/\\0.*$/, '');
    while (src.indexOf('./') === 0) src = src.slice(2);
    if (src.indexOf('/') === 0) src = src.slice(1);
    if (!src) return '';
    if ((src.indexOf('pages/') === 0 || src.indexOf('components/') === 0) && !/\\.mot$/i.test(src)) {
      return src + '.mot';
    }
    return src;
  }

  function buildDebugSourceUrl(sourcePath) {
    var normalized = normalizeSourcePath(sourcePath);
    if (!normalized) return '';
    return normalizeRpcBase(rpcBasePath) + '/source/' + encodeURIComponent(normalized);
  }

  function buildDataRpcUrl(page) {
    return normalizeRpcBase(rpcBasePath) + '/data/' + encodeURIComponent(page);
  }

  function readBytes(memory, ptr, len) {
    var mem = new Uint8Array(memory.buffer);
    if (ptr < 0 || ptr >= mem.length) return new Uint8Array(0);
    var end = Math.min(ptr + len, mem.length);
    return mem.subarray(ptr, end);
  }

  function readCString(memory, ptr, maxLen) {
    var mem = new Uint8Array(memory.buffer);
    if (ptr < 0 || ptr >= mem.length) return '';
    var end = ptr;
    var limit = Math.min(mem.length, ptr + maxLen);
    while (end < limit && mem[end] !== 0) end++;
    return decoder.decode(mem.subarray(ptr, end));
  }

  function writeString(instance, memory, text) {
    var encoded = encoder.encode(String(text == null ? '' : text));
    var ptr = instance.exports.mot_alloc(encoded.length + 1);
    if (!ptr) throw new Error('mot_alloc failed');
    var mem = new Uint8Array(memory.buffer);
    mem.set(encoded, ptr);
    mem[ptr + encoded.length] = 0;
    return { ptr: ptr, len: encoded.length };
  }

  function nodeSignature(node) {
    if (!node) return '';
    if (node.nodeType === Node.TEXT_NODE) return '#text';
    if (node.nodeType === Node.COMMENT_NODE) return '#comment';
    if (node.nodeType === Node.ELEMENT_NODE) return node.tagName;
    return '#other';
  }

  function escapeAttrValue(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/"/g, '&quot;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  function escapeHtmlText(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  function parseSyntheticBindPath(depPath) {
    var normalized = String(depPath || '').trim();
    if (!normalized) return '';
    if (normalized.indexOf('@bind:') === 0) return normalized.slice(6).trim();
    if (normalized.indexOf('@var:') === 0) return normalized.slice(5).trim();
    return '';
  }

  function parseSyntheticSetPath(depPath) {
    var normalized = String(depPath || '').trim();
    if (!normalized) return '';
    if (normalized.indexOf('@set:') === 0) return normalized.slice(5).trim();
    return '';
  }

  function parseSyntheticPlan(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@plan:') !== 0) return null;
    var body = normalized.slice(6);
    var sep = body.indexOf('|');
    if (sep <= 0 || sep >= body.length - 1) return null;
    var setPath = body.slice(0, sep).trim();
    var bindPath = body.slice(sep + 1).trim();
    if (!setPath || !bindPath) return null;
    return { setPath: setPath, bindPath: bindPath };
  }

  function parseSyntheticBindAttr(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@bindattr:') !== 0) return null;
    var body = normalized.slice(10);
    var sep1 = body.indexOf('|');
    if (sep1 <= 0 || sep1 >= body.length - 1) return null;
    var sep2 = body.indexOf('|', sep1 + 1);
    if (sep2 <= sep1 + 1 || sep2 >= body.length - 1) return null;
    var nodeId = body.slice(0, sep1).trim();
    var attrName = body.slice(sep1 + 1, sep2).trim();
    var bindPath = body.slice(sep2 + 1).trim();
    if (!nodeId || !attrName || !bindPath) return null;
    return { nodeId: nodeId, attrName: attrName, bindPath: bindPath };
  }

  function parseSyntheticPlanAttr(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@planattr:') !== 0) return null;
    var body = normalized.slice(10);
    var sep1 = body.indexOf('|');
    if (sep1 <= 0 || sep1 >= body.length - 1) return null;
    var sep2 = body.indexOf('|', sep1 + 1);
    if (sep2 <= sep1 + 1 || sep2 >= body.length - 1) return null;
    var sep3 = body.indexOf('|', sep2 + 1);
    if (sep3 <= sep2 + 1 || sep3 >= body.length - 1) return null;
    var setPath = body.slice(0, sep1).trim();
    var nodeId = body.slice(sep1 + 1, sep2).trim();
    var attrName = body.slice(sep2 + 1, sep3).trim();
    var bindPath = body.slice(sep3 + 1).trim();
    if (!setPath || !nodeId || !attrName || !bindPath) return null;
    return { setPath: setPath, nodeId: nodeId, attrName: attrName, bindPath: bindPath };
  }

  function parseSyntheticExprBind(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@exprbind:') !== 0) return null;
    var body = normalized.slice(10);
    var sep = body.indexOf('|');
    if (sep <= 0 || sep >= body.length - 1) return null;
    var exprId = body.slice(0, sep).trim();
    var program = body.slice(sep + 1).trim();
    if (!exprId || !program) return null;
    return { exprId: exprId, program: program };
  }

  function parseSyntheticExprAttr(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@exprattr:') !== 0) return null;
    var body = normalized.slice(10);
    var sep1 = body.indexOf('|');
    if (sep1 <= 0 || sep1 >= body.length - 1) return null;
    var sep2 = body.indexOf('|', sep1 + 1);
    if (sep2 <= sep1 + 1 || sep2 >= body.length - 1) return null;
    var sep3 = body.indexOf('|', sep2 + 1);
    if (sep3 <= sep2 + 1 || sep3 >= body.length - 1) return null;
    var nodeId = body.slice(0, sep1).trim();
    var attrName = body.slice(sep1 + 1, sep2).trim();
    var exprId = body.slice(sep2 + 1, sep3).trim();
    var program = body.slice(sep3 + 1).trim();
    if (!nodeId || !attrName || !exprId || !program) return null;
    return { nodeId: nodeId, attrName: attrName, exprId: exprId, program: program };
  }

  function parseSyntheticExprDep(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@exprdep:') !== 0) return null;
    var body = normalized.slice(9);
    var sep = body.indexOf('|');
    if (sep <= 0 || sep >= body.length - 1) return null;
    var exprId = body.slice(0, sep).trim();
    var dep = body.slice(sep + 1).trim();
    if (!exprId || !dep) return null;
    return { exprId: exprId, depPath: dep };
  }

  function parseSyntheticPlanExpr(depPath) {
    var normalized = String(depPath || '').trim();
    if (normalized.indexOf('@planexpr:') !== 0) return null;
    var body = normalized.slice(10);
    var sep = body.indexOf('|');
    if (sep <= 0 || sep >= body.length - 1) return null;
    var setPath = body.slice(0, sep).trim();
    var exprId = body.slice(sep + 1).trim();
    if (!setPath || !exprId) return null;
    return { setPath: setPath, exprId: exprId };
  }

  function decodeBase64ToBytes(base64) {
    var text = String(base64 || '');
    if (!text) return new Uint8Array(0);
    var bin = atob(text);
    var out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) {
      out[i] = bin.charCodeAt(i);
    }
    return out;
  }

  function getDebugSourceLinesSync(sourceUrl) {
    var key = String(sourceUrl || '');
    if (!key) return null;
    if (Object.prototype.hasOwnProperty.call(debugSourceLinesCache, key)) {
      return debugSourceLinesCache[key];
    }
    var lines = null;
    try {
      var req = new XMLHttpRequest();
      req.open('GET', key, false);
      req.send(null);
      if (req.status >= 200 && req.status < 300) {
        var body = String(req.responseText || '').replace(/\\r\\n?/g, '\\n');
        lines = body.split('\\n');
      }
    } catch (_) {
      lines = null;
    }
    debugSourceLinesCache[key] = lines;
    return lines;
  }

  function buildPauseScriptWithSource(sourceUrl, line) {
    var safeSourceUrl = String(sourceUrl || '').replace(/[\\r\\n]/g, '').trim();
    if (!safeSourceUrl) return '';
    var safeLine = Math.min(Math.max(Number(line || 0) | 0, 1), 50000);
    var cacheKey = safeSourceUrl + ':' + safeLine;
    if (Object.prototype.hasOwnProperty.call(debugPauseScriptCache, cacheKey)) {
      return debugPauseScriptCache[cacheKey];
    }

    var lines = getDebugSourceLinesSync(safeSourceUrl);
    var script = '';
    if (Array.isArray(lines) && lines.length > 0) {
      var maxLines = Math.min(lines.length, 12000);
      var targetLine = Math.min(safeLine, maxLines);
      var out = new Array(maxLines);
      for (var i = 0; i < maxLines; i++) {
        var raw = String(lines[i] == null ? '' : lines[i]).replace(/[\\r\\n]/g, '');
        out[i] = (i + 1 === targetLine ? 'debugger; // ' : '// ') + raw;
      }
      script = out.join('\\n') + '\\n//# sourceURL=' + safeSourceUrl;
    } else {
      script = '\\n'.repeat(safeLine - 1) + 'debugger;\\n//# sourceURL=' + safeSourceUrl;
    }

    debugPauseScriptCache[cacheKey] = script;
    debugPauseScriptCacheSize += 1;
    if (debugPauseScriptCacheSize > 200) {
      debugPauseScriptCache = Object.create(null);
      debugPauseScriptCacheSize = 0;
    }
    return script;
  }

  function pauseAtSourceStep(event) {
    if (!pauseOnDebugStep) return;
    var line = Number(event && event.line || 0) | 0;
    var sourceUrl = String(event && event.sourceUrl || '').replace(/[\\r\\n]/g, '').trim();
    if (line <= 0 || !sourceUrl || typeof eval !== 'function') {
      debugger;
      return;
    }
    try {
      var script = buildPauseScriptWithSource(sourceUrl, line);
      if (!script) {
        debugger;
        return;
      }
      (0, eval)(script);
    } catch (_) {
      debugger;
    }
  }

  function opcodeName(opcode) {
    var idx = normalizeU32(opcode);
    return OPCODE_NAMES[idx] || ('OP_' + idx);
  }

  function parseJsonOrNull(text) {
    try { return JSON.parse(String(text || '')); } catch (_) { return null; }
  }

  function renderSourceWindow(sourceUrl, line, breakpointsByLine) {
    var lines = getDebugSourceLinesSync(sourceUrl);
    if (!Array.isArray(lines) || lines.length === 0) return '(source unavailable)';
    var target = Math.min(Math.max(Number(line || 0) | 0, 1), lines.length);
    var start = Math.max(1, target - 8);
    var end = Math.min(lines.length, target + 8);
    var width = String(end).length;
    var out = [];
    for (var ln = start; ln <= end; ln++) {
      var marker = ln === target ? '>' : ' ';
      var bp = breakpointsByLine && breakpointsByLine[String(ln)] ? '*' : ' ';
      var no = String(ln);
      while (no.length < width) no = ' ' + no;
      out.push(marker + bp + ' ' + no + ' | ' + lines[ln - 1]);
    }
    return out.join('\\n');
  }

  function ensureStepLiveUiStyle() {
    if (document.getElementById('mot-step-live-style')) return;
    var style = document.createElement('style');
    style.id = 'mot-step-live-style';
    style.textContent =
      '#mot-step-live{position:fixed;top:10px;right:10px;z-index:2147483647;width:520px;max-width:96vw;max-height:95vh;overflow:auto;background:#020617;color:#e2e8f0;border:1px solid #334155;border-radius:10px;padding:10px;font:12px/1.35 ui-monospace,Menlo,Monaco,Consolas,monospace;box-shadow:0 12px 38px rgba(2,6,23,.55);}' +
      '#mot-step-live h3{margin:0 0 6px 0;font-size:13px;color:#f8fafc;}' +
      '#mot-step-live .row{display:flex;gap:6px;align-items:center;margin:0 0 6px 0;}' +
      '#mot-step-live button{border:1px solid #334155;background:#111827;color:#e5e7eb;padding:3px 8px;border-radius:6px;font:12px/1.2 ui-monospace,Menlo,Monaco,Consolas,monospace;cursor:pointer;}' +
      '#mot-step-live button:disabled{opacity:.45;cursor:not-allowed;}' +
      '#mot-step-live input{border:1px solid #334155;background:#020617;color:#e2e8f0;padding:3px 6px;border-radius:6px;font:12px/1.2 ui-monospace,Menlo,Monaco,Consolas,monospace;width:80px;}' +
      '#mot-step-live .status{margin:0 0 4px 0;color:#bfdbfe;}' +
      '#mot-step-live .meta{margin:0 0 6px 0;color:#93c5fd;white-space:normal;word-break:break-word;}' +
      '#mot-step-live pre{margin:0;padding:6px;border-radius:6px;border:1px solid #1f2937;background:#0f172a;color:#e2e8f0;white-space:pre;overflow:auto;max-height:220px;}' +
      '#mot-step-live .split{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:6px;}' +
      '#mot-step-live .label{margin:6px 0 3px 0;color:#93c5fd;}';
    document.head.appendChild(style);
  }

  function formatDebugValue(value, maxLen) {
    var limit = Math.max(32, Number(maxLen || 160) | 0);
    var out = '';
    try {
      out = JSON.stringify(value);
    } catch (_) {
      out = String(value);
    }
    if (out == null) out = String(value);
    out = String(out);
    if (out.length > limit) {
      return out.slice(0, limit - 3) + '...';
    }
    return out;
  }

  function createStepLiveDebuggerUi(page) {
    ensureStepLiveUiStyle();
    var existing = document.getElementById('mot-step-live');
    if (existing && existing.parentNode) {
      existing.parentNode.removeChild(existing);
    }
    var root = document.createElement('aside');
    root.id = 'mot-step-live';
    root.innerHTML =
      '<h3>Mot Live Debug</h3>' +
      '<div class="meta">page: ' + escapeHtmlText(page) + '</div>' +
      '<div class="row">' +
      '<button type="button" data-act="into">Step Into</button>' +
      '<button type="button" data-act="over">Step Over</button>' +
      '<button type="button" data-act="out">Step Out</button>' +
      '<button type="button" data-act="run">Continue</button>' +
      '</div>' +
      '<div class="row">' +
      '<input type="number" min="1" step="1" data-role="bp-line" placeholder="line" />' +
      '<button type="button" data-act="bp-add">+BP</button>' +
      '<button type="button" data-act="bp-del">-BP</button>' +
      '<div class="meta" data-role="bp-list" style="margin:0;">breakpoints: none</div>' +
      '</div>' +
      '<div class="status" data-role="status">ready</div>' +
      '<div class="meta" data-role="meta">No step executed yet.</div>' +
      '<div class="label">Source</div>' +
      '<pre data-role="source">(waiting)</pre>' +
      '<div class="split">' +
      '<div><div class="label">Call Stack</div><pre data-role="stack">(none)</pre></div>' +
      '<div><div class="label">Locals</div><pre data-role="locals">(none)</pre></div>' +
      '</div>';
    document.documentElement.appendChild(root);
    return {
      root: root,
      stepInto: root.querySelector('button[data-act="into"]'),
      stepOver: root.querySelector('button[data-act="over"]'),
      stepOut: root.querySelector('button[data-act="out"]'),
      run: root.querySelector('button[data-act="run"]'),
      bpLine: root.querySelector('[data-role="bp-line"]'),
      bpAdd: root.querySelector('button[data-act="bp-add"]'),
      bpDel: root.querySelector('button[data-act="bp-del"]'),
      bpList: root.querySelector('[data-role="bp-list"]'),
      status: root.querySelector('[data-role="status"]'),
      meta: root.querySelector('[data-role="meta"]'),
      source: root.querySelector('[data-role="source"]'),
      stack: root.querySelector('[data-role="stack"]'),
      locals: root.querySelector('[data-role="locals"]'),
    };
  }

  function setStepLiveUiBusy(ui, busy) {
    if (!ui) return;
    var on = !!busy;
    if (ui.stepInto) ui.stepInto.disabled = on;
    if (ui.stepOver) ui.stepOver.disabled = on;
    if (ui.stepOut) ui.stepOut.disabled = on;
    if (ui.run) ui.run.disabled = on;
    if (ui.bpAdd) ui.bpAdd.disabled = on;
    if (ui.bpDel) ui.bpDel.disabled = on;
  }

  function updateStepLiveUi(ui, info) {
    if (!ui || !info) return;
    var statusText = String(info.status || '');
    var metaText = String(info.meta || '');
    var sourceText = String(info.source || '(source unavailable)');
    var stackText = String(info.stack || '(none)');
    var localsText = String(info.locals || '(none)');
    var breakpointsText = String(info.breakpoints || '');
    if (ui.status) ui.status.textContent = statusText;
    if (ui.meta) ui.meta.textContent = metaText;
    if (ui.source) ui.source.textContent = sourceText;
    if (ui.stack) ui.stack.textContent = stackText;
    if (ui.locals) ui.locals.textContent = localsText;
    if (ui.bpList && breakpointsText) ui.bpList.textContent = breakpointsText;
  }

  function reactivePathFromId(id) {
    if (!reactiveWasmPayload || !reactiveWasmPayload.pathById) return '';
    return String(reactiveWasmPayload.pathById[String(id)] || '');
  }

  function hostGetReactivePathValue(pathId) {
    var path = reactivePathFromId(pathId);
    if (!path) return 0;
    var value = getPathValue(reactiveState, path);
    if (typeof value === 'number') return Number.isFinite(value) ? value : 0;
    if (typeof value === 'boolean') return value ? 1 : 0;
    var parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : 0;
  }

  async function initReactiveWasmModule() {
    if (reactiveWasmExports || reactiveWasmLoading || !reactiveWasmPayload || !reactiveWasmPayload.wasmBase64) {
      return;
    }
    reactiveWasmLoading = true;
    try {
      var wasmBytes = decodeBase64ToBytes(reactiveWasmPayload.wasmBase64);
      if (!wasmBytes.length) return;
      var instantiated = await WebAssembly.instantiate(wasmBytes, {
        env: {
          host_get: function (pathId) {
            return hostGetReactivePathValue(pathId);
          },
        },
      });
      var instance = instantiated.instance || instantiated;
      reactiveWasmExports = instance && instance.exports ? instance.exports : null;
      window.__motReactiveWasm = {
        enabled: !!reactiveWasmExports,
        exprExports: reactiveWasmPayload.exprExports || {},
        pathById: reactiveWasmPayload.pathById || {},
      };
    } catch (err) {
      console.error('[mot-browser-wasm] Failed to init reactive wasm module:', err);
      reactiveWasmExports = null;
    } finally {
      reactiveWasmLoading = false;
    }
  }

  function isObjectLike(value) {
    return value != null && (typeof value === 'object' || typeof value === 'function');
  }

  function syncAttributes(target, source) {
    if (!target || !source || target.nodeType !== Node.ELEMENT_NODE || source.nodeType !== Node.ELEMENT_NODE) {
      return;
    }

    var toRemove = [];
    for (var i = 0; i < target.attributes.length; i++) {
      var attr = target.attributes[i];
      if (!source.hasAttribute(attr.name)) {
        toRemove.push(attr.name);
      }
    }
    for (var r = 0; r < toRemove.length; r++) {
      target.removeAttribute(toRemove[r]);
    }

    for (var j = 0; j < source.attributes.length; j++) {
      var srcAttr = source.attributes[j];
      if (target.getAttribute(srcAttr.name) !== srcAttr.value) {
        target.setAttribute(srcAttr.name, srcAttr.value);
      }
    }
  }

  function morphNode(targetNode, sourceNode) {
    if (!targetNode || !sourceNode) return sourceNode ? sourceNode.cloneNode(true) : null;
    if (targetNode.nodeType !== sourceNode.nodeType || nodeSignature(targetNode) !== nodeSignature(sourceNode)) {
      return sourceNode.cloneNode(true);
    }

    if (targetNode.nodeType === Node.TEXT_NODE || targetNode.nodeType === Node.COMMENT_NODE) {
      if (targetNode.nodeValue !== sourceNode.nodeValue) {
        targetNode.nodeValue = sourceNode.nodeValue;
      }
      return targetNode;
    }

    syncAttributes(targetNode, sourceNode);
    morphChildList(targetNode, sourceNode);
    return targetNode;
  }

  function morphChildList(targetParent, sourceParent) {
    var tChildren = Array.prototype.slice.call(targetParent.childNodes);
    var sChildren = Array.prototype.slice.call(sourceParent.childNodes);
    var max = Math.max(tChildren.length, sChildren.length);

    for (var i = 0; i < max; i++) {
      var tNode = tChildren[i] || null;
      var sNode = sChildren[i] || null;

      if (!sNode && tNode) {
        targetParent.removeChild(tNode);
        continue;
      }

      if (sNode && !tNode) {
        targetParent.appendChild(sNode.cloneNode(true));
        continue;
      }

      var morphed = morphNode(tNode, sNode);
      if (morphed !== tNode && tNode && morphed) {
        targetParent.replaceChild(morphed, tNode);
      }
    }
  }

  function parseRenderedDocument(html) {
    var parser = new DOMParser();
    var parsed = parser.parseFromString(String(html || ''), 'text/html');
    return parsed;
  }

  function stripBootstrapArtifacts(doc) {
    var traceEl = doc.getElementById('mot-trace-data');
    if (traceEl && traceEl.parentNode) traceEl.parentNode.removeChild(traceEl);
    var bootstrapEl = doc.getElementById('mot-browser-wasm-bootstrap');
    if (bootstrapEl && bootstrapEl.parentNode) bootstrapEl.parentNode.removeChild(bootstrapEl);
  }

  function applyRenderedHtmlInPlace(html) {
    var parsed = parseRenderedDocument(html);
    if (!parsed || !parsed.body) return;

    stripBootstrapArtifacts(parsed);

    if (parsed.title && document.title !== parsed.title) {
      document.title = parsed.title;
    }

    morphChildList(document.body, parsed.body);
    rebuildVarBindingIndex();
    installAutoReactiveActions();
  }

  function rebuildVarBindingIndex() {
    var next = Object.create(null);
    var nodes = document.querySelectorAll('[data-mot-bind]');
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      var name = node.getAttribute('data-mot-bind');
      if (!name) continue;
      if (!next[name]) next[name] = [];
      next[name].push(node);
    }
    varBindings = next;
    window.__motVarBindings = varBindings;
    rebuildAttrNodeIndex();
    rebuildExprNodeIndex();
    return varBindings;
  }

  function rebuildAttrNodeIndex() {
    var next = Object.create(null);
    var nodes = document.querySelectorAll('[data-mot-node]');
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      var nodeId = node.getAttribute('data-mot-node');
      if (!nodeId) continue;
      if (!next[nodeId]) next[nodeId] = [];
      next[nodeId].push(node);
    }
    attrNodeIndex = next;
    window.__motReactiveAttrNodes = attrNodeIndex;
    return attrNodeIndex;
  }

  function rebuildExprNodeIndex() {
    var next = Object.create(null);
    var nodes = document.querySelectorAll('[data-mot-expr]');
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i];
      var exprId = node.getAttribute('data-mot-expr');
      if (!exprId) continue;
      if (!next[exprId]) next[exprId] = [];
      next[exprId].push(node);
    }
    exprNodeIndex = next;
    window.__motReactiveExprNodes = exprNodeIndex;
    return exprNodeIndex;
  }

  function resolveNestedValue(baseValue, suffixPath) {
    var suffix = String(suffixPath == null ? '' : suffixPath).trim();
    if (!suffix) return baseValue;
    var parts = suffix.split('.');
    var cur = baseValue;
    for (var i = 0; i < parts.length; i++) {
      var key = parts[i];
      if (!key) continue;
      if (cur == null || (typeof cur !== 'object' && typeof cur !== 'function')) {
        return '';
      }
      cur = cur[key];
    }
    return cur == null ? '' : cur;
  }

  function readU8(view, cursor) {
    if (cursor.value + 1 > view.byteLength) throw new Error('bytecode read overflow (u8)');
    var v = view.getUint8(cursor.value);
    cursor.value += 1;
    return v;
  }

  function readU16(view, cursor) {
    if (cursor.value + 2 > view.byteLength) throw new Error('bytecode read overflow (u16)');
    var v = view.getUint16(cursor.value, true);
    cursor.value += 2;
    return v;
  }

  function readU32(view, cursor) {
    if (cursor.value + 4 > view.byteLength) throw new Error('bytecode read overflow (u32)');
    var v = view.getUint32(cursor.value, true);
    cursor.value += 4;
    return v;
  }

  function skip(view, cursor, len) {
    var n = normalizeU32(len);
    if (cursor.value + n > view.byteLength) throw new Error('bytecode skip overflow');
    cursor.value += n;
  }

  function readLenString(bytes, view, cursor) {
    var len = readU32(view, cursor);
    if (len === 0) return '';
    if (cursor.value + len > view.byteLength) throw new Error('bytecode string overflow');
    var out = decoder.decode(bytes.subarray(cursor.value, cursor.value + len));
    cursor.value += len;
    return out;
  }

  function extractDependencyMarkersFromBytecode(bytecode) {
    try {
      if (!(bytecode instanceof Uint8Array) || bytecode.byteLength < 32) return [];
      var bytes = bytecode;
      var view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      var cursor = { value: 0 };

      var magic = readU32(view, cursor);
      if (magic !== 0x00544F4D) return [];

      readU16(view, cursor); // version major
      readU16(view, cursor); // version minor
      readU16(view, cursor); // flags

      var constCount = readU32(view, cursor);
      var stringCount = readU32(view, cursor);
      var dataReqCount = readU32(view, cursor);
      var depCount = readU32(view, cursor);
      var builtinCount = readU32(view, cursor);
      var compRefCount = readU32(view, cursor);
      var funcCount = readU32(view, cursor);

      for (var i = 0; i < constCount; i++) {
        var type = readU8(view, cursor);
        if (type === 0 || type === 1) {
          readU8(view, cursor);
        } else if (type === 2 || type === 3) {
          skip(view, cursor, 8);
        } else if (type === 4) {
          skip(view, cursor, readU32(view, cursor));
        } else {
          return [];
        }
      }

      for (var s = 0; s < stringCount; s++) {
        skip(view, cursor, readU32(view, cursor));
      }

      for (var d = 0; d < dataReqCount; d++) {
        skip(view, cursor, readU32(view, cursor)); // name
        skip(view, cursor, 1); // is_single
        skip(view, cursor, 1); // is_dynamic
        skip(view, cursor, 4); // query_ref
        skip(view, cursor, 4); // signature
        var paramCount = readU16(view, cursor);
        for (var p = 0; p < paramCount; p++) {
          skip(view, cursor, readU32(view, cursor)); // param_name
          skip(view, cursor, 2); // slot
        }
      }

      var deps = [];
      for (var dep = 0; dep < depCount; dep++) {
        deps.push(readLenString(bytes, view, cursor));
      }

      for (var b = 0; b < builtinCount; b++) {
        skip(view, cursor, readU32(view, cursor)); // name
        skip(view, cursor, 1); // min args
        skip(view, cursor, 1); // max args
      }

      for (var c = 0; c < compRefCount; c++) {
        skip(view, cursor, readU32(view, cursor)); // name
        skip(view, cursor, readU32(view, cursor)); // path
      }

      skip(view, cursor, readU32(view, cursor)); // main chunk
      for (var f = 0; f < funcCount; f++) {
        skip(view, cursor, readU32(view, cursor));
      }

      return deps;
    } catch (_) {
      return [];
    }
  }

  function addUniquePathMap(mapObj, key, value) {
    if (!key || !value) return;
    if (!mapObj[key]) mapObj[key] = [];
    if (mapObj[key].indexOf(value) === -1) {
      mapObj[key].push(value);
    }
  }

  function ruleKey(rule) {
    if (!rule) return '';
    return String(rule.nodeId || '') + '|' + String(rule.attrName || '') + '|' + String(rule.bindPath || '');
  }

  function exprAttrRuleKey(rule) {
    if (!rule) return '';
    return String(rule.nodeId || '') + '|' + String(rule.attrName || '') + '|' + String(rule.exprId || '');
  }

  function addUniqueIdMap(mapObj, key, id) {
    if (!key || !id) return;
    if (!mapObj[key]) mapObj[key] = [];
    if (mapObj[key].indexOf(id) === -1) {
      mapObj[key].push(id);
    }
  }

  function addUniqueExprDepMap(mapObj, exprId, depPath) {
    if (!exprId || !depPath) return;
    if (!mapObj[exprId]) mapObj[exprId] = [];
    if (mapObj[exprId].indexOf(depPath) === -1) {
      mapObj[exprId].push(depPath);
    }
  }

  function addUniqueAttrRuleMap(mapObj, key, rule) {
    if (!key || !rule || !rule.nodeId || !rule.attrName || !rule.bindPath) return;
    if (!mapObj[key]) mapObj[key] = [];
    var keyValue = ruleKey(rule);
    for (var i = 0; i < mapObj[key].length; i++) {
      if (ruleKey(mapObj[key][i]) === keyValue) return;
    }
    mapObj[key].push({ nodeId: rule.nodeId, attrName: rule.attrName, bindPath: rule.bindPath });
  }

  function addUniqueAttrRuleList(list, rule) {
    if (!list || !rule || !rule.nodeId || !rule.attrName || !rule.bindPath) return;
    var keyValue = ruleKey(rule);
    for (var i = 0; i < list.length; i++) {
      if (ruleKey(list[i]) === keyValue) return;
    }
    list.push({ nodeId: rule.nodeId, attrName: rule.attrName, bindPath: rule.bindPath });
  }

  function addUniqueExprAttrRuleMap(mapObj, key, rule) {
    if (!key || !rule || !rule.nodeId || !rule.attrName || !rule.exprId) return;
    if (!mapObj[key]) mapObj[key] = [];
    var keyValue = exprAttrRuleKey(rule);
    for (var i = 0; i < mapObj[key].length; i++) {
      if (exprAttrRuleKey(mapObj[key][i]) === keyValue) return;
    }
    mapObj[key].push({ nodeId: rule.nodeId, attrName: rule.attrName, exprId: rule.exprId });
  }

  function addUniqueExprAttrRuleList(list, rule) {
    if (!list || !rule || !rule.nodeId || !rule.attrName || !rule.exprId) return;
    var keyValue = exprAttrRuleKey(rule);
    for (var i = 0; i < list.length; i++) {
      if (exprAttrRuleKey(list[i]) === keyValue) return;
    }
    list.push({ nodeId: rule.nodeId, attrName: rule.attrName, exprId: rule.exprId });
  }

  function pathMatchesOrPrefix(a, b) {
    var left = String(a == null ? '' : a).trim();
    var right = String(b == null ? '' : b).trim();
    if (!left || !right) return false;
    if (left === right) return true;
    if (left.indexOf(right + '.') === 0) return true;
    if (right.indexOf(left + '.') === 0) return true;
    return false;
  }

  function selectCandidateBindings(mutationPath) {
    var normalized = String(mutationPath == null ? '' : mutationPath).trim();
    if (!normalized) return Object.keys(varBindings);
    return (
      reactiveMeta.setToBind[normalized] ||
      reactiveMeta.rootToBind[normalized] ||
      reactiveMeta.rootToBind[rootOfPath(normalized)] ||
      Object.keys(varBindings)
    );
  }

  function selectCandidateAttrRules(mutationPath) {
    var normalized = String(mutationPath == null ? '' : mutationPath).trim();
    if (!normalized) return [];
    return (
      reactiveMeta.setToAttr[normalized] ||
      reactiveMeta.rootToAttr[normalized] ||
      reactiveMeta.rootToAttr[rootOfPath(normalized)] ||
      []
    );
  }

  function selectCandidateExprIds(mutationPath) {
    var normalized = String(mutationPath == null ? '' : mutationPath).trim();
    if (!normalized) return [];
    return (
      reactiveMeta.setToExpr[normalized] ||
      reactiveMeta.rootToExpr[normalized] ||
      reactiveMeta.rootToExpr[rootOfPath(normalized)] ||
      []
    );
  }

  function rebuildReactiveIndex() {
    var setToBind = Object.create(null);
    var rootToBind = Object.create(null);
    var setToAttr = Object.create(null);
    var rootToAttr = Object.create(null);
    var setToExpr = Object.create(null);
    var rootToExpr = Object.create(null);
    var exprAttrBySet = Object.create(null);
    var exprAttrByRoot = Object.create(null);
    var exprAttrByExprId = Object.create(null);
    var bindPaths = Object.keys(reactiveMeta.bindPaths);
    var setPaths = Object.keys(reactiveMeta.setPaths);
    var exprDepsById = reactiveMeta.exprDepsById || Object.create(null);
    var exprIds = Object.keys(exprDepsById);

    for (var i = 0; i < bindPaths.length; i++) {
      var bindPath = bindPaths[i];
      addUniquePathMap(rootToBind, rootOfPath(bindPath), bindPath);
    }

    var explicitSetPaths = Object.keys(reactiveMeta.planSetToBind);
    for (var p = 0; p < explicitSetPaths.length; p++) {
      var key = explicitSetPaths[p];
      var vals = reactiveMeta.planSetToBind[key] || [];
      for (var pv = 0; pv < vals.length; pv++) {
        addUniquePathMap(setToBind, key, vals[pv]);
      }
    }

    for (var s = 0; s < setPaths.length; s++) {
      var setPath = setPaths[s];
      if (setToBind[setPath] && setToBind[setPath].length > 0) {
        continue;
      }
      for (var j = 0; j < bindPaths.length; j++) {
        var candidate = bindPaths[j];
        if (
          candidate === setPath ||
          candidate.indexOf(setPath + '.') === 0 ||
          setPath.indexOf(candidate + '.') === 0
        ) {
          addUniquePathMap(setToBind, setPath, candidate);
        }
      }
      if (!setToBind[setPath]) {
        var root = rootOfPath(setPath);
        if (rootToBind[root]) {
          setToBind[setPath] = rootToBind[root].slice();
        }
      }
    }

    var bindAttrRules = reactiveMeta.bindAttrRules || [];
    for (var r = 0; r < bindAttrRules.length; r++) {
      var rule = bindAttrRules[r];
      addUniqueAttrRuleMap(rootToAttr, rootOfPath(rule.bindPath), rule);
    }

    var explicitAttrSetPaths = Object.keys(reactiveMeta.planAttrBySet);
    for (var ap = 0; ap < explicitAttrSetPaths.length; ap++) {
      var setKey = explicitAttrSetPaths[ap];
      var explicitRules = reactiveMeta.planAttrBySet[setKey] || [];
      for (var ar = 0; ar < explicitRules.length; ar++) {
        addUniqueAttrRuleMap(setToAttr, setKey, explicitRules[ar]);
      }
    }

    for (var sa = 0; sa < setPaths.length; sa++) {
      var setAttrPath = setPaths[sa];
      if (setToAttr[setAttrPath] && setToAttr[setAttrPath].length > 0) {
        continue;
      }
      for (var br = 0; br < bindAttrRules.length; br++) {
        var bindRule = bindAttrRules[br];
        if (pathMatchesOrPrefix(setAttrPath, bindRule.bindPath)) {
          addUniqueAttrRuleMap(setToAttr, setAttrPath, bindRule);
        }
      }
      if (!setToAttr[setAttrPath]) {
        var rootAttr = rootToAttr[rootOfPath(setAttrPath)];
        if (rootAttr) {
          for (var rr = 0; rr < rootAttr.length; rr++) {
            addUniqueAttrRuleMap(setToAttr, setAttrPath, rootAttr[rr]);
          }
        }
      }
    }

    for (var ei = 0; ei < exprIds.length; ei++) {
      var exprId = exprIds[ei];
      var depList = exprDepsById[exprId] || [];
      for (var ed = 0; ed < depList.length; ed++) {
        addUniqueIdMap(rootToExpr, rootOfPath(depList[ed]), exprId);
      }
    }

    var explicitExprSetPaths = Object.keys(reactiveMeta.planExprBySet);
    for (var es = 0; es < explicitExprSetPaths.length; es++) {
      var setExprKey = explicitExprSetPaths[es];
      var explicitExprs = reactiveMeta.planExprBySet[setExprKey] || [];
      for (var eix = 0; eix < explicitExprs.length; eix++) {
        addUniqueIdMap(setToExpr, setExprKey, explicitExprs[eix]);
      }
    }

    for (var se = 0; se < setPaths.length; se++) {
      var setExprPath = setPaths[se];
      if (setToExpr[setExprPath] && setToExpr[setExprPath].length > 0) {
        continue;
      }
      for (var ee = 0; ee < exprIds.length; ee++) {
        var candidateExprId = exprIds[ee];
        var exprDepList = exprDepsById[candidateExprId] || [];
        for (var ep = 0; ep < exprDepList.length; ep++) {
          if (pathMatchesOrPrefix(setExprPath, exprDepList[ep])) {
            addUniqueIdMap(setToExpr, setExprPath, candidateExprId);
            break;
          }
        }
      }
      if (!setToExpr[setExprPath]) {
        var rootExpr = rootToExpr[rootOfPath(setExprPath)];
        if (rootExpr) {
          setToExpr[setExprPath] = rootExpr.slice();
        }
      }
    }

    var exprAttrRules = reactiveMeta.exprAttrRules || [];
    for (var ea = 0; ea < exprAttrRules.length; ea++) {
      var exprAttrRule = exprAttrRules[ea];
      addUniqueExprAttrRuleMap(exprAttrByExprId, exprAttrRule.exprId, exprAttrRule);
      var exprRuleDeps = exprDepsById[exprAttrRule.exprId] || [];
      for (var erd = 0; erd < exprRuleDeps.length; erd++) {
        addUniqueExprAttrRuleMap(exprAttrByRoot, rootOfPath(exprRuleDeps[erd]), exprAttrRule);
      }
    }

    var setToExprAttr = Object.create(null);
    for (var sb = 0; sb < setPaths.length; sb++) {
      var setExprAttrPath = setPaths[sb];
      var fromIds = setToExpr[setExprAttrPath] || [];
      for (var si = 0; si < fromIds.length; si++) {
        var targetExprId = fromIds[si];
        for (var sr = 0; sr < exprAttrRules.length; sr++) {
          if (exprAttrRules[sr].exprId === targetExprId) {
            addUniqueExprAttrRuleMap(setToExprAttr, setExprAttrPath, exprAttrRules[sr]);
          }
        }
      }
      if (!setToExprAttr[setExprAttrPath]) {
        var rootExprAttr = exprAttrByRoot[rootOfPath(setExprAttrPath)];
        if (rootExprAttr) {
          for (var rx = 0; rx < rootExprAttr.length; rx++) {
            addUniqueExprAttrRuleMap(setToExprAttr, setExprAttrPath, rootExprAttr[rx]);
          }
        }
      }
    }

    reactiveMeta.setToBind = setToBind;
    reactiveMeta.rootToBind = rootToBind;
    reactiveMeta.setToAttr = setToAttr;
    reactiveMeta.rootToAttr = rootToAttr;
    reactiveMeta.setToExpr = setToExpr;
    reactiveMeta.rootToExpr = rootToExpr;
    reactiveMeta.setToExprAttr = setToExprAttr;
    reactiveMeta.exprAttrByExprId = exprAttrByExprId;
    window.__motReactiveMeta = {
      bindPaths: Object.keys(reactiveMeta.bindPaths),
      setPaths: Object.keys(reactiveMeta.setPaths),
      bindAttrRules: reactiveMeta.bindAttrRules,
      exprDepsById: reactiveMeta.exprDepsById,
      exprPrograms: reactiveMeta.exprPrograms,
      exprAttrRules: reactiveMeta.exprAttrRules,
      planSetToBind: reactiveMeta.planSetToBind,
      planAttrBySet: reactiveMeta.planAttrBySet,
      planExprBySet: reactiveMeta.planExprBySet,
      setToBind: reactiveMeta.setToBind,
      rootToBind: reactiveMeta.rootToBind,
      setToAttr: reactiveMeta.setToAttr,
      rootToAttr: reactiveMeta.rootToAttr,
      setToExpr: reactiveMeta.setToExpr,
      rootToExpr: reactiveMeta.rootToExpr,
      setToExprAttr: reactiveMeta.setToExprAttr,
      exprAttrByExprId: reactiveMeta.exprAttrByExprId,
    };
  }

  function registerReactiveMetadata(bytecode) {
    var deps = extractDependencyMarkersFromBytecode(bytecode);
    if (!deps || deps.length === 0) return;

    var changed = false;
    for (var i = 0; i < deps.length; i++) {
      var dep = deps[i];
      var bindPath = parseSyntheticBindPath(dep);
      if (bindPath && !reactiveMeta.bindPaths[bindPath]) {
        reactiveMeta.bindPaths[bindPath] = true;
        changed = true;
      }
      var setPath = parseSyntheticSetPath(dep);
      if (setPath && !reactiveMeta.setPaths[setPath]) {
        reactiveMeta.setPaths[setPath] = true;
        changed = true;
      }
      var bindAttr = parseSyntheticBindAttr(dep);
      if (bindAttr) {
        var beforeBindAttr = reactiveMeta.bindAttrRules.length;
        addUniqueAttrRuleList(reactiveMeta.bindAttrRules, bindAttr);
        if (reactiveMeta.bindAttrRules.length !== beforeBindAttr) changed = true;
      }
      var plan = parseSyntheticPlan(dep);
      if (plan) {
        if (!reactiveMeta.bindPaths[plan.bindPath]) {
          reactiveMeta.bindPaths[plan.bindPath] = true;
          changed = true;
        }
        if (!reactiveMeta.setPaths[plan.setPath]) {
          reactiveMeta.setPaths[plan.setPath] = true;
          changed = true;
        }
        var before = reactiveMeta.planSetToBind[plan.setPath] ? reactiveMeta.planSetToBind[plan.setPath].length : 0;
        addUniquePathMap(reactiveMeta.planSetToBind, plan.setPath, plan.bindPath);
        var after = reactiveMeta.planSetToBind[plan.setPath] ? reactiveMeta.planSetToBind[plan.setPath].length : 0;
        if (after !== before) changed = true;
      }
      var planAttr = parseSyntheticPlanAttr(dep);
      if (planAttr) {
        var beforePlanAttr = reactiveMeta.planAttrBySet[planAttr.setPath] ? reactiveMeta.planAttrBySet[planAttr.setPath].length : 0;
        addUniqueAttrRuleMap(reactiveMeta.planAttrBySet, planAttr.setPath, planAttr);
        var afterPlanAttr = reactiveMeta.planAttrBySet[planAttr.setPath] ? reactiveMeta.planAttrBySet[planAttr.setPath].length : 0;
        if (afterPlanAttr !== beforePlanAttr) changed = true;
        if (!reactiveMeta.bindPaths[planAttr.bindPath]) {
          reactiveMeta.bindPaths[planAttr.bindPath] = true;
          changed = true;
        }
        if (!reactiveMeta.setPaths[planAttr.setPath]) {
          reactiveMeta.setPaths[planAttr.setPath] = true;
          changed = true;
        }
      }
      var exprBind = parseSyntheticExprBind(dep);
      if (exprBind) {
        if (reactiveMeta.exprPrograms[exprBind.exprId] !== exprBind.program) {
          reactiveMeta.exprPrograms[exprBind.exprId] = exprBind.program;
          changed = true;
        }
      }
      var exprAttr = parseSyntheticExprAttr(dep);
      if (exprAttr) {
        if (reactiveMeta.exprPrograms[exprAttr.exprId] !== exprAttr.program) {
          reactiveMeta.exprPrograms[exprAttr.exprId] = exprAttr.program;
          changed = true;
        }
        var beforeExprAttr = reactiveMeta.exprAttrRules.length;
        addUniqueExprAttrRuleList(reactiveMeta.exprAttrRules, exprAttr);
        if (reactiveMeta.exprAttrRules.length !== beforeExprAttr) changed = true;
      }
      var exprDep = parseSyntheticExprDep(dep);
      if (exprDep) {
        var beforeDeps = reactiveMeta.exprDepsById[exprDep.exprId] ? reactiveMeta.exprDepsById[exprDep.exprId].length : 0;
        addUniqueExprDepMap(reactiveMeta.exprDepsById, exprDep.exprId, exprDep.depPath);
        var afterDeps = reactiveMeta.exprDepsById[exprDep.exprId] ? reactiveMeta.exprDepsById[exprDep.exprId].length : 0;
        if (afterDeps !== beforeDeps) changed = true;
      }
      var planExpr = parseSyntheticPlanExpr(dep);
      if (planExpr) {
        if (!reactiveMeta.setPaths[planExpr.setPath]) {
          reactiveMeta.setPaths[planExpr.setPath] = true;
          changed = true;
        }
        var beforePlanExpr = reactiveMeta.planExprBySet[planExpr.setPath] ? reactiveMeta.planExprBySet[planExpr.setPath].length : 0;
        addUniqueIdMap(reactiveMeta.planExprBySet, planExpr.setPath, planExpr.exprId);
        var afterPlanExpr = reactiveMeta.planExprBySet[planExpr.setPath] ? reactiveMeta.planExprBySet[planExpr.setPath].length : 0;
        if (afterPlanExpr !== beforePlanExpr) changed = true;
      }
    }

    if (changed) {
      rebuildReactiveIndex();
    }
  }

  function applyTextToBoundNodes(nodes, value) {
    var text = String(value == null ? '' : value);
    for (var i = 0; i < nodes.length; i++) {
      nodes[i].textContent = text;
    }
  }

  function enqueueMutation(path, value, rootPath, rootValue) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return;
    if (!Object.prototype.hasOwnProperty.call(mutationQueue, normalized)) {
      mutationQueueOrder.push(normalized);
    }
    mutationQueue[normalized] = {
      value: value,
      rootPath: String(rootPath == null ? '' : rootPath).trim(),
      rootValue: rootValue,
    };
  }

  function flushMutationQueue() {
    mutationFlushScheduled = false;
    if (!mutationQueueOrder.length) return false;

    var order = mutationQueueOrder.slice();
    mutationQueueOrder.length = 0;
    var queue = mutationQueue;
    mutationQueue = Object.create(null);

    var didUpdate = false;
    for (var i = 0; i < order.length; i++) {
      var key = order[i];
      var entry = queue[key];
      if (!entry) continue;
      didUpdate = updateVarBindings(key, entry.value, entry.rootPath, entry.rootValue) || didUpdate;
    }
    return didUpdate;
  }

  function scheduleMutationFlush() {
    if (mutationFlushScheduled) return;
    mutationFlushScheduled = true;
    if (typeof queueMicrotask === 'function') {
      queueMicrotask(flushMutationQueue);
      return;
    }
    Promise.resolve().then(flushMutationQueue);
  }

  function valueForBindPath(bindPath, mutationPath, mutationValue, rootPath, rootValue) {
    if (bindPath === mutationPath) {
      return mutationValue;
    }
    if (bindPath.indexOf(mutationPath + '.') === 0) {
      return resolveNestedValue(mutationValue, bindPath.slice(mutationPath.length + 1));
    }
    if (rootPath && rootPath.length > 0) {
      if (bindPath === rootPath) {
        return rootValue;
      }
      if (bindPath.indexOf(rootPath + '.') === 0) {
        return resolveNestedValue(rootValue, bindPath.slice(rootPath.length + 1));
      }
    }
    return undefined;
  }

  function setPathValue(target, path, value) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return;
    var parts = normalized.split('.');
    var cur = target;
    for (var i = 0; i < parts.length - 1; i++) {
      var key = parts[i];
      if (!key) continue;
      if (!isObjectLike(cur[key])) {
        cur[key] = {};
      }
      cur = cur[key];
    }
    var last = parts[parts.length - 1];
    if (!last) return;
    cur[last] = value;
  }

  function getPathValue(target, path) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return undefined;
    var parts = normalized.split('.');
    var cur = target;
    for (var i = 0; i < parts.length; i++) {
      var key = parts[i];
      if (!key) continue;
      if (cur == null || (typeof cur !== 'object' && typeof cur !== 'function')) {
        return undefined;
      }
      cur = cur[key];
    }
    return cur;
  }

  function setReactiveStateForMutation(path, value, rootPath, rootValue) {
    var normalized = String(path == null ? '' : path).trim();
    var normalizedRoot = String(rootPath == null ? '' : rootPath).trim();
    if (normalizedRoot && rootValue !== undefined) {
      reactiveState[normalizedRoot] = rootValue;
    }
    if (!normalized) return;
    if (normalizedRoot && normalized === normalizedRoot && rootValue !== undefined) {
      return;
    }
    setPathValue(reactiveState, normalized, value);
  }

  function decodeTokenValue(raw) {
    var text = String(raw == null ? '' : raw);
    if (!text) return '';
    try {
      return decodeURIComponent(text);
    } catch (_) {
      return text;
    }
  }

  function valueTruthy(value) {
    if (value == null) return false;
    if (typeof value === 'boolean') return value;
    if (typeof value === 'number') return value !== 0;
    if (typeof value === 'string') return value.length > 0;
    if (Array.isArray(value)) return value.length > 0;
    if (typeof value === 'object') return Object.keys(value).length > 0;
    return !!value;
  }

  function valuesEqual(a, b) {
    if (a === b) return true;
    var aNum = typeof a === 'number';
    var bNum = typeof b === 'number';
    if (aNum && bNum) return a === b;
    if (aNum && typeof b === 'string' && b.trim() !== '') {
      var bn = Number(b);
      return Number.isFinite(bn) && a === bn;
    }
    if (bNum && typeof a === 'string' && a.trim() !== '') {
      var an = Number(a);
      return Number.isFinite(an) && b === an;
    }
    return false;
  }

  function applyBinaryOp(op, left, right) {
    switch (op) {
      case 'add':
        if (typeof left === 'number' && typeof right === 'number') {
          return left + right;
        }
        return String(left == null ? '' : left) + String(right == null ? '' : right);
      case 'sub': return Number(left || 0) - Number(right || 0);
      case 'mul': return Number(left || 0) * Number(right || 0);
      case 'div': return Number(left || 0) / Number(right || 0);
      case 'mod': return Number(left || 0) % Number(right || 0);
      case 'lt': return Number(left || 0) < Number(right || 0);
      case 'lte': return Number(left || 0) <= Number(right || 0);
      case 'gt': return Number(left || 0) > Number(right || 0);
      case 'gte': return Number(left || 0) >= Number(right || 0);
      case 'eq': return valuesEqual(left, right);
      case 'neq': return !valuesEqual(left, right);
      case 'and': return valueTruthy(left) && valueTruthy(right);
      case 'or': return valueTruthy(left) || valueTruthy(right);
      default: return undefined;
    }
  }

  function evaluateReactiveProgram(program) {
    var src = String(program == null ? '' : program);
    if (!src) return undefined;
    var tokens = src.split(';');
    var stack = [];

    for (var i = 0; i < tokens.length; i++) {
      var token = tokens[i];
      if (!token) continue;
      if (token === 'Z') {
        stack.push(null);
        continue;
      }
      if (token === 'T') {
        var elseValue = stack.length ? stack.pop() : undefined;
        var thenValue = stack.length ? stack.pop() : undefined;
        var condValue = stack.length ? stack.pop() : undefined;
        stack.push(valueTruthy(condValue) ? thenValue : elseValue);
        continue;
      }
      if (token === 'X') {
        var indexValue = stack.length ? stack.pop() : undefined;
        var objectValue = stack.length ? stack.pop() : undefined;
        if (objectValue == null || (typeof objectValue !== 'object' && typeof objectValue !== 'function')) {
          stack.push(undefined);
        } else {
          stack.push(objectValue[indexValue]);
        }
        continue;
      }

      var sep = token.indexOf(':');
      var head = sep >= 0 ? token.slice(0, sep) : token;
      var tail = sep >= 0 ? token.slice(sep + 1) : '';
      var decodedTail = decodeTokenValue(tail);

      if (head === 'B') {
        stack.push(decodedTail === '1');
        continue;
      }
      if (head === 'I') {
        var intValue = parseInt(decodedTail, 10);
        stack.push(Number.isFinite(intValue) ? intValue : 0);
        continue;
      }
      if (head === 'N') {
        var numberValue = Number(decodedTail);
        stack.push(Number.isFinite(numberValue) ? numberValue : 0);
        continue;
      }
      if (head === 'S') {
        stack.push(decodedTail);
        continue;
      }
      if (head === 'P') {
        stack.push(getPathValue(reactiveState, decodedTail));
        continue;
      }
      if (head === 'M') {
        var parent = stack.length ? stack.pop() : undefined;
        if (parent == null || (typeof parent !== 'object' && typeof parent !== 'function')) {
          stack.push(undefined);
        } else {
          stack.push(parent[decodedTail]);
        }
        continue;
      }
      if (head === 'U') {
        var unaryValue = stack.length ? stack.pop() : undefined;
        if (decodedTail === 'not') {
          stack.push(!valueTruthy(unaryValue));
        } else if (decodedTail === 'neg') {
          stack.push(-Number(unaryValue || 0));
        } else {
          stack.push(undefined);
        }
        continue;
      }
      if (head === 'O') {
        var right = stack.length ? stack.pop() : undefined;
        var left = stack.length ? stack.pop() : undefined;
        stack.push(applyBinaryOp(decodedTail, left, right));
        continue;
      }
    }

    return stack.length ? stack[stack.length - 1] : undefined;
  }

  function evaluateReactiveExpr(exprId, program) {
    if (
      reactiveWasmExports &&
      reactiveWasmPayload &&
      reactiveWasmPayload.exprExports
    ) {
      var exportName = reactiveWasmPayload.exprExports[String(exprId)];
      if (exportName && typeof reactiveWasmExports[exportName] === 'function') {
        try {
          return reactiveWasmExports[exportName]();
        } catch (_) {
          // Fall back to JS evaluator if wasm execution fails for a specific expr.
        }
      }
    }
    return evaluateReactiveProgram(program);
  }

  function updateVarBindings(name, value, rootPath, rootValue) {
    var normalizedName = String(name == null ? '' : name).trim();
    if (!normalizedName) return false;
    setReactiveStateForMutation(normalizedName, value, rootPath, rootValue);
    if (!Object.keys(varBindings).length) {
      rebuildVarBindingIndex();
    }
    if (!Object.keys(attrNodeIndex).length) {
      rebuildAttrNodeIndex();
    }
    if (!Object.keys(exprNodeIndex).length) {
      rebuildExprNodeIndex();
    }

    var candidatePaths = selectCandidateBindings(normalizedName);
    var didUpdate = false;
    for (var i = 0; i < candidatePaths.length; i++) {
      var bindPath = candidatePaths[i];
      var list = varBindings[bindPath];
      if (!list || list.length === 0) continue;
      var nextValue = valueForBindPath(bindPath, normalizedName, value, rootPath, rootValue);
      if (nextValue === undefined) continue;
      applyTextToBoundNodes(list, nextValue);
      didUpdate = true;
    }

    var candidateAttrRules = selectCandidateAttrRules(normalizedName);
    for (var a = 0; a < candidateAttrRules.length; a++) {
      var rule = candidateAttrRules[a];
      var els = attrNodeIndex[rule.nodeId];
      if (!els || els.length === 0) continue;
      var attrValue = valueForBindPath(rule.bindPath, normalizedName, value, rootPath, rootValue);
      if (attrValue === undefined) continue;
      var attrText = String(attrValue == null ? '' : attrValue);
      for (var e = 0; e < els.length; e++) {
        els[e].setAttribute(rule.attrName, attrText);
      }
      didUpdate = true;
    }

    var candidateExprIds = selectCandidateExprIds(normalizedName);
    for (var x = 0; x < candidateExprIds.length; x++) {
      var exprId = candidateExprIds[x];
      var program = reactiveMeta.exprPrograms[exprId];
      if (!program) continue;
      var exprValue = evaluateReactiveExpr(exprId, program);

      var exprNodes = exprNodeIndex[exprId];
      if (exprNodes && exprNodes.length > 0) {
        applyTextToBoundNodes(exprNodes, exprValue);
        didUpdate = true;
      }

      var exprAttrRules = reactiveMeta.exprAttrByExprId[exprId] || [];
      for (var xr = 0; xr < exprAttrRules.length; xr++) {
        var exprAttrRule = exprAttrRules[xr];
        var exprAttrNodes = attrNodeIndex[exprAttrRule.nodeId];
        if (!exprAttrNodes || exprAttrNodes.length === 0) continue;
        var exprAttrText = String(exprValue == null ? '' : exprValue);
        for (var xe = 0; xe < exprAttrNodes.length; xe++) {
          exprAttrNodes[xe].setAttribute(exprAttrRule.attrName, exprAttrText);
        }
        didUpdate = true;
      }
    }
    return didUpdate;
  }

  function ensureSignal(path) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return null;
    if (signalRegistry[normalized]) return signalRegistry[normalized];
    var initial = getPathValue(reactiveState, normalized);
    if (typeof initial !== 'number') {
      var maybe = Number(initial);
      initial = Number.isFinite(maybe) ? maybe : 0;
    }
    var created = window.motSignal(normalized, initial);
    signalRegistry[normalized] = created;
    return created;
  }

  function applyReactivePathValue(path, value) {
    var normalized = String(path == null ? '' : path).trim();
    if (!normalized) return false;
    var root = rootOfPath(normalized);
    if (!root) return false;
    if (normalized === root) {
      setReactiveStateForMutation(normalized, value, normalized, value);
      return updateVarBindings(normalized, value, normalized, value);
    }
    if (!isObjectLike(reactiveState[root])) {
      reactiveState[root] = {};
    }
    setPathValue(reactiveState, normalized, value);
    return updateVarBindings(normalized, value, root, reactiveState[root]);
  }

  function installAutoReactiveActions() {
    var buttons = document.querySelectorAll('[data-mot-inc]');
    for (var i = 0; i < buttons.length; i++) {
      var button = buttons[i];
      if (button.getAttribute('data-mot-bound') === '1') continue;
      button.setAttribute('data-mot-bound', '1');
      button.addEventListener('click', function () {
        var counterPath = this.getAttribute('data-mot-inc');
        var signal = ensureSignal(counterPath);
        if (!signal) return;
        var next = signal.set(signal.get() + 1);

        var setPath = this.getAttribute('data-mot-set-path');
        if (setPath) {
          var prefix = this.getAttribute('data-mot-set-prefix') || '';
          applyReactivePathValue(setPath, String(prefix) + String(next));
        }
      });
    }
  }

  function installReactiveApi() {
    window.motSetVar = function (name, value) {
      return updateVarBindings(name, value, '', undefined);
    };
    window.motFlushReactivity = function () {
      return flushMutationQueue();
    };
    window.motRefreshBindings = function () {
      return rebuildVarBindingIndex();
    };
    window.motListBindings = function () {
      if (!Object.keys(varBindings).length) {
        rebuildVarBindingIndex();
      }
      return Object.keys(varBindings);
    };
    window.motSignal = function (name, initialValue) {
      var normalizedName = String(name == null ? '' : name).trim();
      var current = initialValue;
      if (normalizedName) {
        setReactiveStateForMutation(normalizedName, current, normalizedName, current);
      }
      return {
        get: function () {
          return current;
        },
        set: function (nextValue) {
          current = nextValue;
          if (!normalizedName) return current;
          enqueueMutation(normalizedName, current, normalizedName, current);
          scheduleMutationFlush();
          return current;
        },
      };
    };
    window.motCreateState = function (name, initialValue) {
      var normalizedName = String(name == null ? '' : name).trim();
      if (!normalizedName) return initialValue;
      if (typeof Proxy === 'undefined' || !isObjectLike(initialValue)) {
        setReactiveStateForMutation(normalizedName, initialValue, normalizedName, initialValue);
        return initialValue;
      }

      var proxyCache = typeof WeakMap !== 'undefined' ? new WeakMap() : null;
      var rootRef = { value: initialValue };

      function notifyMutation(path, value) {
        enqueueMutation(path, value, normalizedName, rootRef.value);
        scheduleMutationFlush();
      }

      function wrap(value, absPath) {
        if (!isObjectLike(value)) return value;
        if (proxyCache && proxyCache.has(value)) {
          return proxyCache.get(value);
        }
        var proxy = new Proxy(value, {
          get: function (target, prop, receiver) {
            var out = Reflect.get(target, prop, receiver);
            if (typeof prop === 'symbol') return out;
            var nextPath = absPath ? (absPath + '.' + String(prop)) : String(prop);
            return wrap(out, nextPath);
          },
          set: function (target, prop, nextValue, receiver) {
            var nextPath = absPath ? (absPath + '.' + String(prop)) : String(prop);
            var wrapped = wrap(nextValue, nextPath);
            var ok = Reflect.set(target, prop, wrapped, receiver);
            notifyMutation(nextPath, wrapped);
            return ok;
          },
          deleteProperty: function (target, prop) {
            var nextPath = absPath ? (absPath + '.' + String(prop)) : String(prop);
            var ok = Reflect.deleteProperty(target, prop);
            notifyMutation(nextPath, '');
            return ok;
          },
        });
        if (proxyCache) {
          proxyCache.set(value, proxy);
        }
        return proxy;
      }

      rootRef.value = wrap(rootRef.value, normalizedName);
      notifyMutation(normalizedName, rootRef.value);
      return rootRef.value;
    };

    window.motInstallActions = function () {
      installAutoReactiveActions();
    };
  }

  function normalizeDataRequirement(queryRef, signature, isSingle, name) {
    return {
      queryRef: normalizeU32(queryRef),
      signature: normalizeU32(signature),
      isSingle: !!isSingle,
      name: String(name || ''),
    };
  }

  async function getRuntimeWasmBytes() {
    if (!runtimeWasmBytesPromise) {
      runtimeWasmBytesPromise = fetch(buildRuntimeWasmRpcUrl())
        .then(function (res) {
          if (!res.ok) {
            throw new Error('Failed to fetch runtime wasm: status=' + res.status);
          }
          return res.arrayBuffer();
        })
        .then(function (buffer) {
          return new Uint8Array(buffer);
        })
        .catch(function (err) {
          runtimeWasmBytesPromise = null;
          throw err;
        });
    }
    return runtimeWasmBytesPromise;
  }

  async function fetchPageBytecode(page) {
    var key = String(page || '');
    if (!pageBytecodeCache[key] && key === pageName && initialPageBytecodeBase64) {
      pageBytecodeCache[key] = Promise.resolve(decodeBase64ToBytes(initialPageBytecodeBase64));
    }
    if (!pageBytecodeCache[key]) {
      pageBytecodeCache[key] = fetch(buildPageRpcUrl(page))
        .then(function (response) {
          if (!response.ok) {
            throw new Error('Failed to fetch page bytecode: page=' + page + ' status=' + response.status);
          }
          return response.arrayBuffer();
        })
        .then(function (buffer) {
          return new Uint8Array(buffer);
        })
        .catch(function (err) {
          delete pageBytecodeCache[key];
          throw err;
        });
    }
    return pageBytecodeCache[key];
  }

  async function fetchComponentBytecode(componentPath) {
    var normalized = normalizeComponentPath(componentPath);
    if (!normalized) {
      throw new Error('Invalid component path: ' + String(componentPath || ''));
    }
    if (!componentBytecodeCache[normalized]) {
      componentBytecodeCache[normalized] = fetch(buildComponentRpcUrl(normalized))
        .then(function (response) {
          if (!response.ok) {
            throw new Error('Failed to fetch component bytecode: path=' + normalized + ' status=' + response.status);
          }
          return response.arrayBuffer();
        })
        .then(function (buffer) {
          return new Uint8Array(buffer);
        })
        .catch(function (err) {
          delete componentBytecodeCache[normalized];
          throw err;
        });
    }
    return componentBytecodeCache[normalized];
  }

  async function fetchData(req, params) {
    var response = await fetch(buildDataRpcUrl(pageName), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        queryRef: req.queryRef,
        signature: req.signature,
        params: params && typeof params === 'object' ? params : {},
        single: req.isSingle,
      }),
    });
    if (!response.ok) {
      var detail = '';
      try { detail = await response.text(); } catch (_) { detail = ''; }
      throw new Error('Data RPC failed: status=' + response.status + ' body=' + detail);
    }
    var json = await response.json();
    if (req.isSingle && Array.isArray(json)) {
      return json[0] || null;
    }
    return json;
  }

  async function renderBytecode(bytecode, componentOptions) {
    registerReactiveMetadata(bytecode);
    var interactiveStepMode = stepLiveMode && !componentOptions;
    var state = {
      instance: null,
      memory: null,
      html: '',
      error: '',
      depStack: [],
      pendingResume: null,
      stepCount: 0,
      lastLineByChunk: Object.create(null),
      lastDebugEvent: null,
      lastSnapshot: null,
      debugBusy: false,
      debugResolve: null,
      debugReject: null,
      debugUi: null,
      debugDone: false,
      breakpointsBySource: Object.create(null),
      activeSourcePath: 'pages/' + pageName + '.mot',
    };

    function frameDepth(snapshot) {
      var frames = snapshot && Array.isArray(snapshot.frames) ? snapshot.frames : null;
      return frames ? frames.length : 0;
    }

    function readDebugSnapshot() {
      if (!state.instance || !state.instance.exports) return null;
      if (!state.instance.exports.mot_debug_snapshot_ptr || !state.instance.exports.mot_debug_snapshot_len) {
        return null;
      }
      var len = Number(state.instance.exports.mot_debug_snapshot_len() || 0) | 0;
      if (len <= 0) return null;
      var ptr = Number(state.instance.exports.mot_debug_snapshot_ptr() || 0) | 0;
      if (ptr <= 0) return null;
      var json = decoder.decode(readBytes(state.memory, ptr, len));
      return parseJsonOrNull(json);
    }

    function getBreakpointsForSource(sourcePath) {
      var normalized = normalizeSourcePath(sourcePath);
      if (!normalized) return Object.create(null);
      if (!state.breakpointsBySource[normalized]) {
        state.breakpointsBySource[normalized] = Object.create(null);
      }
      return state.breakpointsBySource[normalized];
    }

    function formatBreakpointSummary(sourcePath) {
      var table = getBreakpointsForSource(sourcePath);
      var lines = Object.keys(table)
        .map(function (key) { return Number(key) | 0; })
        .filter(function (n) { return n > 0; })
        .sort(function (a, b) { return a - b; });
      if (lines.length === 0) return 'breakpoints: none';
      return 'breakpoints (' + sourcePath + '): ' + lines.join(', ');
    }

    function sourcePointFromState(snapshot, event) {
      var snap = snapshot || {};
      var frames = Array.isArray(snap.frames) ? snap.frames : [];
      var activeFrame = frames.length > 0 ? frames[frames.length - 1] : null;
      var depth = frames.length;
      var chunkKind = activeFrame && activeFrame.kind === 'main' ? 0 : 1;
      var chunkIndex = activeFrame ? normalizeU32(activeFrame.kind === 'main' ? 0 : activeFrame.funcIdx) : 0;
      var chunkKey = chunkKind + ':' + chunkIndex;
      var sourcePath = normalizeSourcePath(
        (activeFrame && activeFrame.sourcePath) ||
        (event && event.sourcePath) ||
        ('pages/' + pageName + '.mot'),
      ) || ('pages/' + pageName + '.mot');
      var line = 0;
      var column = 0;
      if (event &&
          event.chunkKind === chunkKind &&
          event.chunkIndex === chunkIndex &&
          Number(event.line || 0) > 0) {
        line = Number(event.line) | 0;
        column = Number(event.column || 0) | 0;
      } else if (Object.prototype.hasOwnProperty.call(state.lastLineByChunk, chunkKey)) {
        line = Number(state.lastLineByChunk[chunkKey] || 0) | 0;
      } else if (event && Number(event.line || 0) > 0) {
        line = Number(event.line) | 0;
        column = Number(event.column || 0) | 0;
      }
      if (line <= 0) line = 1;
      return {
        sourcePath: sourcePath,
        line: line,
        column: column,
        depth: depth,
        chunkKind: chunkKind,
        chunkIndex: chunkIndex,
        pc: Number(event && event.pc || (activeFrame && activeFrame.pc || 0)) | 0,
        opcode: Number(event && event.opcode || (activeFrame && activeFrame.nextOpcode || 0)) | 0,
        valid: !!sourcePath && line > 0,
      };
    }

    function sameSourceLocation(a, b) {
      if (!a || !b) return false;
      return a.sourcePath === b.sourcePath &&
        a.line === b.line &&
        a.column === b.column;
    }

    function setBreakpoint(sourcePath, line, enabled) {
      var normalized = normalizeSourcePath(sourcePath);
      var ln = Number(line) | 0;
      if (!normalized || ln <= 0) return;
      var table = getBreakpointsForSource(normalized);
      if (enabled) {
        table[String(ln)] = true;
      } else {
        delete table[String(ln)];
      }
    }

    function hasBreakpoint(point) {
      if (!point || !point.valid) return false;
      var table = getBreakpointsForSource(point.sourcePath);
      return !!table[String(point.line)];
    }

    function snapshotUiInfo(statusLabel) {
      var snapshot = state.lastSnapshot || readDebugSnapshot() || {};
      var frames = Array.isArray(snapshot.frames) ? snapshot.frames : [];
      var activeFrame = frames.length > 0 ? frames[frames.length - 1] : null;
      var event = state.lastDebugEvent;
      var point = sourcePointFromState(snapshot, event);
      var sourcePath = point.sourcePath;
      var line = point.line;
      var column = point.column;
      state.activeSourcePath = sourcePath;
      var sourceUrl = buildDebugSourceUrl(sourcePath);
      var sourceText = renderSourceWindow(
        sourceUrl || sourcePath,
        line,
        getBreakpointsForSource(sourcePath),
      );
      var stackText = '(no frames)';
      if (frames.length > 0) {
        stackText = frames.map(function (frame, idx) {
          var frameSource = normalizeSourcePath(frame && frame.sourcePath || '') || ('pages/' + pageName + '.mot');
          return (
            '#' + idx +
            ' ' + String(frame && frame.kind || 'unknown') +
            ' fn=' + String(frame && frame.funcIdx != null ? frame.funcIdx : '-') +
            ' pc=' + String(frame && frame.pc != null ? frame.pc : '-') +
            ' op=' + opcodeName(frame && frame.nextOpcode != null ? frame.nextOpcode : 0) +
            ' src=' + frameSource
          );
        }).join('\\n');
      }
      var localsText = '(no locals)';
      var activeLocals = activeFrame && Array.isArray(activeFrame.locals) ? activeFrame.locals : null;
      if (activeLocals && activeLocals.length > 0) {
        localsText = activeLocals.slice(0, 96).map(function (item) {
          return 'slot ' + String(item && item.slot != null ? item.slot : '?') +
            ' = ' + formatDebugValue(item ? item.value : null, 220);
        }).join('\\n');
      }
      var opcode = event
        ? opcodeName(event.opcode)
        : opcodeName(activeFrame && activeFrame.nextOpcode != null ? activeFrame.nextOpcode : 0);
      return {
        status: String(statusLabel || 'paused') +
          ' | steps=' + String(state.stepCount) +
          ' | depth=' + String(frames.length) +
          ' | op=' + opcode,
        meta: 'source=' + sourcePath +
          ':' + String(line) +
          ':' + String(column) +
          ' | pc=' + String(point.pc) +
          ' | done=' + String(!!snapshot.done) +
          ' | vmState=' + String(snapshot.state == null ? '-' : snapshot.state),
        source: sourceText,
        stack: stackText,
        locals: localsText,
        breakpoints: formatBreakpointSummary(sourcePath),
        point: point,
      };
    }

    async function resumeWithPayload(reqId, payloadText, stepResume) {
      var payload = writeString(state.instance, state.memory, payloadText);
      if (stepResume && state.instance.exports.mot_resume_step) {
        return state.instance.exports.mot_resume_step(reqId, payload.ptr, payload.len);
      }
      return state.instance.exports.mot_resume(reqId, payload.ptr, payload.len);
    }

    async function consumePendingResume(stepResume) {
      var pending = state.pendingResume;
      if (!pending) {
        throw new Error(state.error || 'WASM requested resume without pending async operation');
      }
      state.pendingResume = null;
      var payload = await pending;
      if (!payload || typeof payload.reqId !== 'number') {
        throw new Error(state.error || 'Invalid pending resume payload');
      }
      return resumeWithPayload(payload.reqId >>> 0, String(payload.payloadText || ''), !!stepResume);
    }

    function finalizeDependencyStack() {
      while (state.depStack.length > 0) {
        var dep = state.depStack.pop();
        if (dep && dep.kind === 'bind') {
          state.html += '</span>';
        }
      }
    }

    async function executeOneVmStep() {
      state.lastDebugEvent = null;
      var rc = state.instance.exports.mot_step();
      while (rc === MOT_AWAIT) {
        rc = await consumePendingResume(true);
      }
      if (rc !== MOT_OK) {
        throw new Error(state.error || ('WASM step failed (' + rc + ')'));
      }
      state.lastSnapshot = readDebugSnapshot();
      window.__motLiveStepSnapshot = state.lastSnapshot;
      return {
        snapshot: state.lastSnapshot,
        event: state.lastDebugEvent,
        done: !!(state.lastSnapshot && state.lastSnapshot.done),
      };
    }

    async function runStepCommand(mode) {
      if (!state.debugUi) return;
      if (state.debugBusy || state.debugDone) return;
      state.debugBusy = true;
      setStepLiveUiBusy(state.debugUi, true);
      updateStepLiveUi(state.debugUi, snapshotUiInfo('running ' + mode));
      try {
        var currentSnapshot = state.lastSnapshot || readDebugSnapshot() || {};
        var startPoint = sourcePointFromState(currentSnapshot, state.lastDebugEvent);
        var startDepth = frameDepth(currentSnapshot);
        var iterations = 0;
        var done = false;
        var stopReason = 'paused';
        var lastLocationKey = startPoint.sourcePath + ':' + startPoint.line + ':' + startPoint.column;
        while (!done) {
          var result = await executeOneVmStep();
          iterations++;
          var point = sourcePointFromState(result.snapshot, result.event);
          var depth = point.depth;
          var locationChanged = !sameSourceLocation(point, startPoint);
          var movedPastStart = locationChanged || depth !== startDepth;
          done = !!result.done;
          if (mode === 'into') {
            if (!done && movedPastStart) done = true;
          } else if (mode === 'over') {
            if (!done && depth < startDepth) done = true;
            if (!done && depth === startDepth && locationChanged) done = true;
          } else if (mode === 'out') {
            if (!done && depth < startDepth) done = true;
          } else if (mode === 'run') {
            if (!done && point.valid) {
              var key = point.sourcePath + ':' + point.line + ':' + point.column;
              if (key !== lastLocationKey && hasBreakpoint(point)) {
                done = true;
                stopReason = 'breakpoint';
              }
              lastLocationKey = key;
            }
          } else {
            done = true;
          }
          if (done && result.done) stopReason = 'done';
          if (iterations > 200000) {
            throw new Error('Step execution limit reached');
          }
        }
        var finished = !!(state.lastSnapshot && state.lastSnapshot.done);
        updateStepLiveUi(state.debugUi, snapshotUiInfo(finished ? 'done' : stopReason));
        if (finished) {
          state.debugDone = true;
          setStepLiveUiBusy(state.debugUi, true);
          if (state.debugResolve) {
            var resolve = state.debugResolve;
            state.debugResolve = null;
            resolve();
          }
        }
      } catch (err) {
        updateStepLiveUi(state.debugUi, {
          status: 'error',
          meta: String(err && err.message || err),
          source: '(source unavailable)',
          stack: '(unavailable)',
          locals: '(unavailable)',
        });
        if (state.debugReject) {
          var reject = state.debugReject;
          state.debugReject = null;
          reject(err);
        }
        throw err;
      } finally {
        state.debugBusy = false;
        if (!state.debugDone) {
          setStepLiveUiBusy(state.debugUi, false);
        }
      }
    }

    var imports = {
      env: {
        host_output: function (ptr, len) {
          state.html += decoder.decode(readBytes(state.memory, ptr, len));
        },
        host_output_text: function (ptr, len) {
          state.html += escapeHtmlText(decoder.decode(readBytes(state.memory, ptr, len)));
        },
        host_error: function (ptr, len) {
          state.error = decoder.decode(readBytes(state.memory, ptr, len));
        },
        host_log: function (ptr, len) {
          var msg = decoder.decode(readBytes(state.memory, ptr, len));
          console.log('[mot-browser-wasm]', msg);
        },
        host_render_complete: function () {},
        host_dep_start: function (ptr, len) {
          var depPath = decoder.decode(readBytes(state.memory, ptr, len));
          var bindPath = parseSyntheticBindPath(depPath);
          if (bindPath) {
            state.html += '<span data-mot-bind="' + escapeAttrValue(bindPath) + '">';
            state.depStack.push({ kind: 'bind' });
            return;
          }
          state.depStack.push({ kind: 'dep' });
        },
        host_dep_end: function () {
          var dep = state.depStack.length > 0 ? state.depStack.pop() : null;
          if (dep && dep.kind === 'bind') {
            state.html += '</span>';
          }
        },
        host_component_start: function (_funcIdx) {},
        host_component_end: function (_funcIdx) {},
        host_slot_default_start: function (_funcIdx) {},
        host_slot_default_end: function (_funcIdx) {},
        host_fetch_data: function (reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
          if (state.pendingResume) {
            state.pendingResume = Promise.reject(new Error('Concurrent async fetch is not supported'));
            return;
          }
          var reqIdU32 = normalizeU32(reqId);
          var req = normalizeDataRequirement(
            queryRef,
            signature,
            isSingle,
            readCString(state.memory, namePtr, 256),
          );
          var paramsJson = readCString(state.memory, paramsPtr, 4096);
          var params = {};
          try {
            params = paramsJson ? JSON.parse(paramsJson) : {};
          } catch (_) {
            params = {};
          }
          state.pendingResume = Promise.resolve(fetchData(req, params)).then(function (data) {
            return {
              reqId: reqIdU32,
              payloadText: JSON.stringify(data == null ? null : data),
            };
          });
        },
        host_load_component: function (reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
          if (state.pendingResume) {
            state.pendingResume = Promise.reject(new Error('Concurrent async component load is not supported'));
            return;
          }
          var reqIdU32 = normalizeU32(reqId);
          var name = readCString(state.memory, namePtr, 256);
          var path = readCString(state.memory, pathPtr, 512);
          var componentPath = normalizeComponentPath(path || name);
          var propsJson = readCString(state.memory, propsPtr, 4096);
          var childrenJson = readCString(state.memory, childrenPtr, 4096);
          var props = null;
          var children = null;
          try { props = propsJson ? JSON.parse(propsJson) : null; } catch (_) { props = null; }
          try { children = childrenJson ? JSON.parse(childrenJson) : null; } catch (_) { children = null; }
          if (!componentPath) {
            state.pendingResume = Promise.reject(new Error('Invalid component ref'));
            return;
          }
          state.pendingResume = Promise.resolve(fetchComponentBytecode(componentPath))
            .then(function (componentBytecode) {
              return renderBytecode(componentBytecode, {
                functionIndex: 0,
                props: props,
                children: children,
              });
            })
            .then(function (html) {
              return {
                reqId: reqIdU32,
                payloadText: String(html || ''),
              };
            });
        },
        host_load_component_linked: function (reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
          imports.env.host_load_component(reqId, namePtr, pathPtr, propsPtr, childrenPtr);
        },
        host_debug_step: function (chunkKind, chunkIndex, pc, opcode, line, column, sourcePathPtr, sourcePathLen) {
          if (!stepLiveMode) return;
          var normalizedChunkKind = normalizeU32(chunkKind);
          var normalizedChunkIndex = normalizeU32(chunkIndex);
          var normalizedLine = normalizeU32(line);
          var chunkKey = normalizedChunkKind + ':' + normalizedChunkIndex;
          if (normalizedLine > 0) {
            state.lastLineByChunk[chunkKey] = normalizedLine;
          } else if (Object.prototype.hasOwnProperty.call(state.lastLineByChunk, chunkKey)) {
            normalizedLine = state.lastLineByChunk[chunkKey];
          }
          var normalizedSource = normalizeSourcePath(
            decoder.decode(readBytes(state.memory, sourcePathPtr, sourcePathLen)),
          ) || ('pages/' + pageName + '.mot');
          var event = {
            index: state.stepCount++,
            chunkKind: normalizedChunkKind,
            chunkIndex: normalizedChunkIndex,
            pc: normalizeU32(pc),
            opcode: normalizeU32(opcode),
            line: normalizedLine,
            column: normalizeU32(column),
            sourcePath: normalizedSource,
            sourceUrl: buildDebugSourceUrl(normalizedSource) || normalizedSource,
          };
          state.lastDebugEvent = event;
          window.__motLiveStepLast = event;
          if (pauseOnDebugStep && !interactiveStepMode) {
            pauseAtSourceStep(event);
          }
        },
      },
    };

    var wasmBytes = await getRuntimeWasmBytes();
    var instantiated = await WebAssembly.instantiate(wasmBytes, imports);
    var instance = instantiated.instance || instantiated;
    if (!instance || !instance.exports || !instance.exports.memory) {
      throw new Error('Failed to instantiate browser wasm runtime');
    }

    state.instance = instance;
    state.memory = instance.exports.memory;

    if (!instance.exports.mot_alloc || !instance.exports.mot_init || !instance.exports.mot_render) {
      throw new Error('Missing required wasm exports');
    }

    if (instance.exports.mot_reset_alloc) {
      instance.exports.mot_reset_alloc();
    }

    var bytecodePtr = instance.exports.mot_alloc(bytecode.length);
    if (!bytecodePtr) {
      throw new Error('mot_alloc failed for bytecode');
    }
    new Uint8Array(state.memory.buffer).set(bytecode, bytecodePtr);

    var initRc = instance.exports.mot_init(bytecodePtr, bytecode.length);
    if (initRc !== MOT_OK) {
      throw new Error(state.error || ('mot_init failed (' + initRc + ')'));
    }

    if (instance.exports.mot_set_debug_step_mode) {
      instance.exports.mot_set_debug_step_mode(stepLiveMode ? 1 : 0);
    }
    if (instance.exports.mot_set_debug_component_markers) {
      instance.exports.mot_set_debug_component_markers(stepLiveMode ? 1 : 0);
    }

    try {
      var rc;
      if (componentOptions) {
        if (!instance.exports.mot_render_component) {
          throw new Error('Missing mot_render_component export');
        }
        var funcIndex = Number(componentOptions.functionIndex || 0);
        var propsPayload = writeString(instance, state.memory, JSON.stringify(componentOptions.props == null ? null : componentOptions.props));
        var childrenPayload = writeString(instance, state.memory, JSON.stringify(componentOptions.children == null ? null : componentOptions.children));
        rc = instance.exports.mot_render_component(funcIndex >>> 0, propsPayload.ptr, propsPayload.len, childrenPayload.ptr, childrenPayload.len);
        while (rc === MOT_AWAIT) {
          rc = await consumePendingResume(false);
        }
        if (rc !== MOT_OK) {
          throw new Error(state.error || ('WASM render_component failed (' + rc + ')'));
        }
      } else if (!interactiveStepMode) {
        rc = instance.exports.mot_render();
        while (rc === MOT_AWAIT) {
          rc = await consumePendingResume(false);
        }
        if (rc !== MOT_OK) {
          throw new Error(state.error || ('WASM render failed (' + rc + ')'));
        }
      } else {
        if (!instance.exports.mot_step || !instance.exports.mot_resume_step) {
          throw new Error('Step-live requires mot_step and mot_resume_step exports');
        }
        if (!instance.exports.mot_debug_snapshot_ptr || !instance.exports.mot_debug_snapshot_len) {
          throw new Error('Step-live requires mot_debug_snapshot exports');
        }
        state.debugUi = createStepLiveDebuggerUi(pageName);
        state.lastSnapshot = readDebugSnapshot();
        updateStepLiveUi(state.debugUi, snapshotUiInfo('paused'));
        setStepLiveUiBusy(state.debugUi, false);

        state.debugUi.stepInto.addEventListener('click', function () {
          runStepCommand('into').catch(function (err) {
            console.error('[mot-browser-wasm] Step Into failed:', err);
          });
        });
        state.debugUi.stepOver.addEventListener('click', function () {
          runStepCommand('over').catch(function (err) {
            console.error('[mot-browser-wasm] Step Over failed:', err);
          });
        });
        state.debugUi.stepOut.addEventListener('click', function () {
          runStepCommand('out').catch(function (err) {
            console.error('[mot-browser-wasm] Step Out failed:', err);
          });
        });
        state.debugUi.run.addEventListener('click', function () {
          runStepCommand('run').catch(function (err) {
            console.error('[mot-browser-wasm] Continue failed:', err);
          });
        });
        state.debugUi.bpAdd.addEventListener('click', function () {
          var line = Number(state.debugUi.bpLine && state.debugUi.bpLine.value || 0) | 0;
          if (line <= 0) return;
          setBreakpoint(state.activeSourcePath, line, true);
          updateStepLiveUi(state.debugUi, snapshotUiInfo(state.debugDone ? 'done' : 'paused'));
        });
        state.debugUi.bpDel.addEventListener('click', function () {
          var line = Number(state.debugUi.bpLine && state.debugUi.bpLine.value || 0) | 0;
          if (line <= 0) return;
          setBreakpoint(state.activeSourcePath, line, false);
          updateStepLiveUi(state.debugUi, snapshotUiInfo(state.debugDone ? 'done' : 'paused'));
        });
        state.debugUi.bpLine.addEventListener('keydown', function (event) {
          if (event && event.key === 'Enter') {
            event.preventDefault();
            var line = Number(state.debugUi.bpLine && state.debugUi.bpLine.value || 0) | 0;
            if (line <= 0) return;
            setBreakpoint(state.activeSourcePath, line, true);
            updateStepLiveUi(state.debugUi, snapshotUiInfo(state.debugDone ? 'done' : 'paused'));
          }
        });

        window.__motLiveStepController = {
          stepInto: function () { return runStepCommand('into'); },
          stepOver: function () { return runStepCommand('over'); },
          stepOut: function () { return runStepCommand('out'); },
          continue: function () { return runStepCommand('run'); },
          addBreakpoint: function (sourcePath, line) { setBreakpoint(sourcePath || state.activeSourcePath, line, true); },
          removeBreakpoint: function (sourcePath, line) { setBreakpoint(sourcePath || state.activeSourcePath, line, false); },
          snapshot: function () { return state.lastSnapshot; },
          lastEvent: function () { return state.lastDebugEvent; },
        };

        await new Promise(function (resolve, reject) {
          state.debugResolve = resolve;
          state.debugReject = reject;
        });
      }

      finalizeDependencyStack();
      return state.html;
    } finally {
      if (instance.exports.mot_free) {
        instance.exports.mot_free();
      }
    }
  }

  installReactiveApi();
  initReactiveWasmModule();

  (async function () {
    try {
      var pageBytecode = await fetchPageBytecode(pageName);
      var html = await renderBytecode(pageBytecode, null);
      applyRenderedHtmlInPlace(html);
    } catch (err) {
      console.error('[mot-browser-wasm] Failed browser-side render:', err);
    }
  })();
})();
</script>`;
}
