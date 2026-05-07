export function renderDevtoolsBootstrapScript() {
  return `<script>
(function () {
  var traceEl = document.getElementById('mot-trace-data');
  if (!traceEl) return;

  var trace = {};
  try { trace = JSON.parse(traceEl.textContent || '{}'); } catch (_) { trace = {}; }

  function text(v) { return v == null ? '' : String(v); }
  function esc(v) {
    return text(v)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }
  function list(items, render) {
    if (!Array.isArray(items) || items.length === 0) return '<li>none</li>';
    return items.slice(0, 40).map(render).join('');
  }
  function pageSource() {
    var sourceMap = trace.bytecodeDebug && trace.bytecodeDebug.sourceMap;
    var sources = sourceMap && sourceMap.sources;
    if (Array.isArray(sources) && sources.length > 0 && sources[0]) return String(sources[0]);
    return 'pages/' + text(trace.page || 'index') + '.mot';
  }
  function normalizeSourcePath(sourcePath) {
    var src = text(sourcePath).trim();
    while (src.indexOf('./') === 0) {
      src = src.slice(2);
    }
    if (src.indexOf('/') === 0) {
      src = src.slice(1);
    }
    if (!src) return '';
    if ((src.indexOf('components/') === 0 || src.indexOf('pages/') === 0) && !src.endsWith('.mot')) {
      return src + '.mot';
    }
    return src;
  }
  function deriveComponentPathFromSource(sourcePath) {
    var src = normalizeSourcePath(sourcePath);
    if (!src) return '';
    if (src.endsWith('.mot')) return src.slice(0, -4);
    return src;
  }
  function buildComponentMetaByFunction() {
    var out = {};
    var list = trace.bytecodeDebug && trace.bytecodeDebug.functionComponents;
    if (!Array.isArray(list)) return out;
    for (var i = 0; i < list.length; i++) {
      var raw = list[i] || {};
      var chunkKind = Number(raw.chunkKind == null ? 1 : raw.chunkKind) >>> 0;
      var chunkIndex = Number(raw.chunkIndex == null ? 0 : raw.chunkIndex) >>> 0;
      if (chunkKind !== 1) continue;
      var source = normalizeSourcePath(raw.sourcePath || '');
      if (!source) continue;
      var componentPath = deriveComponentPathFromSource(source);
      var componentName = raw.name ? String(raw.name) : (componentPath || ('component#' + chunkIndex));
      out[String(chunkIndex)] = {
        source: source,
        component: componentPath || componentName,
        render: 'wasm-vm',
        load: 'static',
        artifact: 'inline',
        opcode: 'BC_COMPONENT_START',
      };
    }
    return out;
  }
  function parseBoundaryMarker(commentValue) {
    var value = text(commentValue).trim();
    var componentMatch = /^motc:(\\d+):(start|end)$/.exec(value);
    if (componentMatch) {
      return { kind: 'component', idx: componentMatch[1], phase: componentMatch[2] };
    }
    var slotMatch = /^motslot:(\\d+):(start|end)$/.exec(value);
    if (slotMatch) {
      return { kind: 'slot', idx: slotMatch[1], phase: slotMatch[2] };
    }
    return null;
  }

  var pageMeta = {
    source: pageSource(),
    component: 'pages/' + text(trace.page || 'index'),
    render: text(trace.vmUsed || trace.vmMode || 'unknown'),
    load: 'page',
    artifact: 'origin',
    opcode: 'n/a',
  };
  var componentMetaByFunction = buildComponentMetaByFunction();
  var vmStepDebug = trace.vmStepDebug && typeof trace.vmStepDebug === 'object' ? trace.vmStepDebug : null;
  var vmSteps = vmStepDebug && Array.isArray(vmStepDebug.steps) ? vmStepDebug.steps : [];
  var vmStepSources = vmStepDebug && vmStepDebug.sources && typeof vmStepDebug.sources === 'object'
    ? vmStepDebug.sources
    : {};

  function resolveInfoFromMarkers(targetNode) {
    var rootNode = document.documentElement;
    var target = targetNode && targetNode.nodeType === 1 ? targetNode : null;
    if (!rootNode || !target) return pageMeta;

    var componentStack = [];
    var slotSuspendedStack = [];
    var walker = document.createTreeWalker(rootNode, NodeFilter.SHOW_ALL);
    var node = walker.currentNode;
    while (node) {
      if (node === target) break;
      if (node.nodeType === Node.COMMENT_NODE) {
        var marker = parseBoundaryMarker(node.nodeValue || '');
        if (marker) {
          if (marker.kind === 'component') {
            if (marker.phase === 'start') {
              componentStack.push(marker.idx);
            } else {
              for (var i = componentStack.length - 1; i >= 0; i--) {
                if (componentStack[i] === marker.idx) {
                  componentStack.splice(i, 1);
                  break;
                }
              }
            }
          } else if (marker.phase === 'start') {
            var top = componentStack.length > 0 ? componentStack[componentStack.length - 1] : null;
            if (top && top === marker.idx) {
              slotSuspendedStack.push(top);
              componentStack.pop();
            } else {
              slotSuspendedStack.push(null);
            }
          } else {
            var suspended = slotSuspendedStack.length > 0 ? slotSuspendedStack.pop() : null;
            if (suspended) componentStack.push(suspended);
          }
        }
      }
      node = walker.nextNode();
    }
    var activeIdx = componentStack.length > 0 ? componentStack[componentStack.length - 1] : null;
    if (activeIdx && componentMetaByFunction[activeIdx]) return componentMetaByFunction[activeIdx];
    return pageMeta;
  }

  var root = document.documentElement;
  if (root) {
    root.setAttribute('data-mot-source', pageSource());
    root.setAttribute('data-mot-component', '(page)');
    root.setAttribute('data-mot-render', text(trace.vmUsed || trace.vmMode || 'unknown'));
    root.setAttribute('data-mot-load', 'page');
    root.setAttribute('data-mot-opcode', 'n/a');
    root.setAttribute('data-mot-artifact', 'origin');
  }

  var style = document.createElement('style');
  style.textContent =
    '#mot-debug-toggle{position:fixed;right:12px;bottom:12px;z-index:2147483647;border:0;background:#111827;color:#fff;padding:8px 10px;border-radius:8px;font:12px/1.2 monospace;cursor:pointer;}' +
    '#mot-debug-panel{position:fixed;top:0;right:0;z-index:2147483646;width:420px;max-width:92vw;height:100vh;overflow:auto;background:#0b1220;color:#e5e7eb;border-left:1px solid #374151;padding:12px;font:12px/1.4 ui-monospace,Menlo,Monaco,Consolas,monospace;display:none;}' +
    '#mot-debug-panel h3{margin:0 0 8px 0;font-size:13px;color:#f9fafb;}' +
    '#mot-debug-panel h4{margin:10px 0 4px 0;font-size:12px;color:#93c5fd;}' +
    '#mot-debug-panel ul{margin:0;padding-left:16px;}' +
    '#mot-debug-panel code{color:#fca5a5;}' +
    '#mot-debug-actions{display:flex;gap:8px;margin:8px 0 10px 0;}' +
    '#mot-debug-actions button{border:1px solid #374151;background:#111827;color:#e5e7eb;padding:4px 8px;border-radius:6px;font:12px/1.2 ui-monospace,Menlo,Monaco,Consolas,monospace;cursor:pointer;}' +
    '#mot-debug-actions button[data-active="1"]{background:#1d4ed8;border-color:#60a5fa;color:#eff6ff;}' +
    '#mot-debug-inspect-status{margin:0 0 8px 0;color:#bfdbfe;}' +
    '#mot-step-debugger{border:1px solid #1f2937;border-radius:8px;padding:8px;background:#020617;margin:6px 0 10px 0;}' +
    '#mot-step-controls{display:flex;gap:6px;align-items:center;margin-bottom:6px;}' +
    '#mot-step-controls button{border:1px solid #334155;background:#111827;color:#e5e7eb;padding:2px 7px;border-radius:5px;font:12px/1.2 ui-monospace,Menlo,Monaco,Consolas,monospace;cursor:pointer;}' +
    '#mot-step-controls input[type="range"]{flex:1;}' +
    '#mot-step-status{margin:0 0 5px 0;color:#bfdbfe;}' +
    '#mot-step-source-meta{margin:0 0 4px 0;color:#93c5fd;}' +
    '#mot-step-source,#mot-step-scope{margin:0;padding:6px;border:1px solid #1f2937;border-radius:6px;background:#0f172a;color:#e2e8f0;white-space:pre;overflow:auto;max-height:220px;font:11px/1.35 ui-monospace,Menlo,Monaco,Consolas,monospace;}' +
    '#mot-step-scope{margin-top:6px;max-height:120px;}' +
    '#mot-step-truncated{margin:5px 0 0 0;color:#fca5a5;}' +
    '#mot-debug-outline{position:fixed;z-index:2147483645;pointer-events:none;border:2px dashed #22d3ee;background:rgba(34,211,238,0.08);display:none;}' +
    '#mot-debug-tooltip{position:fixed;z-index:2147483645;pointer-events:none;max-width:340px;padding:6px 8px;border-radius:6px;border:1px solid #0f172a;background:#020617;color:#e2e8f0;font:11px/1.35 ui-monospace,Menlo,Monaco,Consolas,monospace;display:none;white-space:normal;}';
  document.head.appendChild(style);

  var btn = document.createElement('button');
  btn.id = 'mot-debug-toggle';
  btn.textContent = 'MOT Debug';

  var panel = document.createElement('aside');
  panel.id = 'mot-debug-panel';
  panel.innerHTML =
    '<h3>Motus Debug</h3>' +
    '<div><b>page:</b> ' + esc(trace.page) + '</div>' +
    '<div><b>vm:</b> ' + esc(trace.vmUsed || trace.vmMode) + '</div>' +
    '<div><b>duration:</b> ' + esc(trace.totalDurationMs) + 'ms</div>' +
    '<div><b>bytecode:</b> ' + esc(trace.bytecodeSize) + ' bytes</div>' +
    '<div id="mot-debug-actions">' +
      '<button id="mot-debug-inspect-toggle" type="button">Inspect</button>' +
    '</div>' +
    '<div id="mot-debug-inspect-status">inspect: off</div>' +
    '<h4>Step Debugger</h4>' +
    (vmSteps.length > 0
      ? '<div id="mot-step-debugger">' +
          '<div id="mot-step-controls">' +
            '<button id="mot-step-prev" type="button">Prev</button>' +
            '<button id="mot-step-next" type="button">Next</button>' +
            '<input id="mot-step-range" type="range" min="0" max="' + esc(Math.max(0, vmSteps.length - 1)) + '" value="0" />' +
          '</div>' +
          '<div id="mot-step-status"></div>' +
          '<div id="mot-step-source-meta"></div>' +
          '<pre id="mot-step-source"></pre>' +
          '<pre id="mot-step-scope"></pre>' +
          (vmStepDebug && vmStepDebug.truncated
            ? '<div id="mot-step-truncated">Step trace truncated at ' + esc(vmStepDebug.maxSteps) + ' events.</div>'
            : '') +
        '</div>'
      : '<div>none</div>') +
    '<h4>Component Renders</h4><ul>' +
      list(trace.componentRenders, function (c) {
        return '<li>' + esc(c.path) + ' vm=' + esc(c.renderEngine) + ' load=' + esc(c.loadSource) +
          ' op=' + esc(c.componentOpcode || 'n/a') +
          ' artifact=' + esc(c.artifactSource) + ' ' + esc(c.durationMs) + 'ms</li>';
      }) +
    '</ul>' +
    '<h4>Component Loads</h4><ul>' +
      list(trace.componentLoads, function (c) {
        return '<li>' + esc(c.path) + ' [' + esc(c.source) + ']' +
          ' artifact=' + esc(c.artifactSource) + ' ' + esc(c.durationMs) + 'ms</li>';
      }) +
    '</ul>' +
    '<h4>Data Fetches</h4><ul>' +
      list(trace.dataFetches, function (f) {
        return '<li><code>q' + esc(f.queryRef) + '</code> ' + esc(f.durationMs) + 'ms rows=' + esc(f.rowCount) + '</li>';
      }) +
    '</ul>' +
    '<h4>Imports</h4><ul>' +
      list(trace.bytecodeDebug && trace.bytecodeDebug.componentRefs, function (c) {
        return '<li>' + esc(c.path || c.name) + '</li>';
      }) +
    '</ul>' +
    '<h4>Queries</h4><ul>' +
      list(trace.bytecodeDebug && trace.bytecodeDebug.queries, function (q) {
        return '<li><code>' + esc(q.name) + '</code> line ' + esc(q.line) + '</li>';
      }) +
    '</ul>' +
    '<h4>Render Spans</h4><ul>' +
      list(trace.bytecodeDebug && trace.bytecodeDebug.spans, function (s) {
        return '<li>chunk=' + esc(s.chunkKind) + ':' + esc(s.chunkIndex) + ' pc ' + esc(s.startPc) + '-' + esc(s.endPc) + ' line ' + esc(s.line) + '</li>';
      }) +
    '</ul>' +
    '<h4>Static Components</h4><ul>' +
      list(trace.bytecodeDebug && trace.bytecodeDebug.functionComponents, function (fc) {
        return '<li>fn=' + esc(fc.chunkIndex) + ' ' + esc(fc.name || '') + ' source=' + esc(fc.sourcePath || '') + '</li>';
      }) +
    '</ul>' +
    '<h4>Source Map</h4><ul>' +
      (trace.bytecodeDebug && trace.bytecodeDebug.sourceMap
        ? '<li>file=' + esc(trace.bytecodeDebug.sourceMap.file) + ' sources=' + esc((trace.bytecodeDebug.sourceMap.sources || []).length) + ' mappings=' + esc((trace.bytecodeDebug.sourceMap.mappings || '').length) + ' chars</li>'
        : '<li>none</li>') +
    '</ul>' +
    '<h4>Component Maps</h4><ul>' +
      list(Object.entries(trace.componentSourceMaps || {}), function (entry) {
        var compPath = entry[0];
        var info = entry[1] || {};
        var map = info.sourceMap || {};
        return '<li>' + esc(compPath) + ' [' + esc(info.source || 'unknown') + '] file=' + esc(map.file || '') + '</li>';
      }) +
    '</ul>';

  var outline = document.createElement('div');
  outline.id = 'mot-debug-outline';

  var tooltip = document.createElement('div');
  tooltip.id = 'mot-debug-tooltip';

  var inspectButton = null;
  var inspectStatus = null;
  var stepPrevButton = null;
  var stepNextButton = null;
  var stepRange = null;
  var stepStatus = null;
  var stepSourceMeta = null;
  var stepSource = null;
  var stepScope = null;
  var currentStepIndex = 0;
  var stepSourceLinesCache = {};
  var inspectEnabled = false;
  var lastInfoTarget = null;
  var lastInfoValue = null;

  function infoForElement(el) {
    if (el && el === lastInfoTarget && lastInfoValue) return lastInfoValue;
    var resolved = resolveInfoFromMarkers(el);
    if (resolved && resolved.source) {
      var info = {
        source: resolved.source || pageMeta.source,
        component: resolved.component || '(page)',
        render: resolved.render || pageMeta.render,
        load: resolved.load || 'unknown',
        artifact: resolved.artifact || 'unknown',
        opcode: resolved.opcode || 'n/a',
      };
      lastInfoTarget = el;
      lastInfoValue = info;
      return info;
    }
    var fallback = {
      source: pageMeta.source,
      component: '(page)',
      render: pageMeta.render,
      load: 'page',
      artifact: 'origin',
      opcode: 'n/a',
    };
    lastInfoTarget = el;
    lastInfoValue = fallback;
    return fallback;
  }
  function renderTooltip(info) {
    return 'source: ' + text(info.source) + '<br>' +
      'component: ' + text(info.component) + '<br>' +
      'render: ' + text(info.render) + ' | load: ' + text(info.load) +
      ' | opcode: ' + text(info.opcode) + ' | artifact: ' + text(info.artifact);
  }

  function sourceLinesFor(path) {
    var key = normalizeSourcePath(path);
    if (!key) return [];
    if (!Object.prototype.hasOwnProperty.call(stepSourceLinesCache, key)) {
      var raw = text(vmStepSources[key] || '');
      stepSourceLinesCache[key] = raw ? raw.split(/\\r?\\n/) : [];
    }
    return stepSourceLinesCache[key];
  }

  function renderSourceWindow(path, line) {
    var lines = sourceLinesFor(path);
    if (!Array.isArray(lines) || lines.length === 0) {
      return '(source unavailable)';
    }
    var targetLine = Number(line || 0) | 0;
    var start = targetLine > 0 ? Math.max(1, targetLine - 8) : 1;
    var end = targetLine > 0
      ? Math.min(lines.length, targetLine + 8)
      : Math.min(lines.length, 16);
    var width = String(end).length;
    var out = [];
    for (var ln = start; ln <= end; ln++) {
      var marker = ln === targetLine ? '>' : ' ';
      var lineNo = String(ln);
      while (lineNo.length < width) lineNo = ' ' + lineNo;
      out.push(marker + ' ' + lineNo + ' | ' + lines[ln - 1]);
    }
    return out.join('\\n');
  }

  function renderStepDebugger() {
    if (!stepStatus || vmSteps.length === 0) return;
    var idx = Number(currentStepIndex) | 0;
    if (idx < 0) idx = 0;
    if (idx >= vmSteps.length) idx = vmSteps.length - 1;
    currentStepIndex = idx;
    var step = vmSteps[idx] || {};
    var line = Number(step.line || 0) >>> 0;
    var column = Number(step.column || 0) >>> 0;
    var sourcePath = normalizeSourcePath(step.sourcePath || pageMeta.source);
    var opcodeLabel = step.opcodeName ? text(step.opcodeName) : ('OP_' + text(step.opcode));

    if (stepRange) {
      stepRange.value = String(idx);
      stepRange.max = String(Math.max(0, vmSteps.length - 1));
    }

    stepStatus.innerHTML =
      '#' + esc(idx + 1) + '/' + esc(vmSteps.length) +
      ' ' + esc(opcodeLabel) +
      ' chunk=' + esc(step.chunkKind) + ':' + esc(step.chunkIndex) +
      ' pc=' + esc(step.pc) +
      ' +' + esc(Number(step.atMs || 0).toFixed(2)) + 'ms';

    if (stepSourceMeta) {
      stepSourceMeta.textContent = sourcePath
        ? (sourcePath + (line > 0 ? (':' + line + ':' + column) : ''))
        : 'source unavailable';
    }
    if (stepSource) {
      stepSource.textContent = renderSourceWindow(sourcePath, line);
    }
    if (stepScope) {
      var scope = step.scope && typeof step.scope === 'object' ? step.scope : {};
      var scopeLines = [
        'component: ' + text(scope.componentPath || '(page)'),
        'callKind: ' + text(scope.callKind || 'page'),
        'componentOpcode: ' + text(scope.componentOpcode || 'n/a'),
        'loadSource: ' + text(scope.loadSource || 'page'),
        'renderEngine: ' + text(scope.renderEngine || 'wasm-vm'),
      ];
      if (Array.isArray(scope.scopeStack) && scope.scopeStack.length > 0) {
        scopeLines.push('stack: ' + scope.scopeStack.join(' > '));
      }
      stepScope.textContent = scopeLines.join('\\n');
    }
  }

  function placeTooltip(event) {
    var x = event.clientX + 14;
    var y = event.clientY + 14;
    var maxX = window.innerWidth - tooltip.offsetWidth - 8;
    var maxY = window.innerHeight - tooltip.offsetHeight - 8;
    tooltip.style.left = Math.max(8, Math.min(maxX, x)) + 'px';
    tooltip.style.top = Math.max(8, Math.min(maxY, y)) + 'px';
  }

  function handleInspectMove(event) {
    if (!inspectEnabled) return;
    var target = event.target;
    if (!target || target === panel || panel.contains(target) || target === btn) {
      outline.style.display = 'none';
      tooltip.style.display = 'none';
      return;
    }

    var rect = target.getBoundingClientRect();
    if (!rect || rect.width <= 0 || rect.height <= 0) {
      outline.style.display = 'none';
      tooltip.style.display = 'none';
      return;
    }
    outline.style.display = 'block';
    outline.style.left = rect.left + 'px';
    outline.style.top = rect.top + 'px';
    outline.style.width = rect.width + 'px';
    outline.style.height = rect.height + 'px';

    var info = infoForElement(target);
    tooltip.innerHTML = renderTooltip(info);
    tooltip.style.display = 'block';
    placeTooltip(event);
  }

  function handleInspectClick(event) {
    if (!inspectEnabled) return;
    var target = event.target;
    if (!target || target === panel || panel.contains(target) || target === btn) return;
    event.preventDefault();
    event.stopPropagation();
  }

  function setInspectEnabled(next) {
    inspectEnabled = !!next;
    if (inspectButton) inspectButton.setAttribute('data-active', inspectEnabled ? '1' : '0');
    if (inspectStatus) inspectStatus.textContent = 'inspect: ' + (inspectEnabled ? 'on' : 'off');
    if (!inspectEnabled) {
      outline.style.display = 'none';
      tooltip.style.display = 'none';
    }
  }

  btn.addEventListener('click', function () {
    panel.style.display = panel.style.display === 'block' ? 'none' : 'block';
  });

  document.body.appendChild(btn);
  document.body.appendChild(panel);
  document.body.appendChild(outline);
  document.body.appendChild(tooltip);

  inspectButton = document.getElementById('mot-debug-inspect-toggle');
  inspectStatus = document.getElementById('mot-debug-inspect-status');
  stepPrevButton = document.getElementById('mot-step-prev');
  stepNextButton = document.getElementById('mot-step-next');
  stepRange = document.getElementById('mot-step-range');
  stepStatus = document.getElementById('mot-step-status');
  stepSourceMeta = document.getElementById('mot-step-source-meta');
  stepSource = document.getElementById('mot-step-source');
  stepScope = document.getElementById('mot-step-scope');
  if (inspectButton) {
    inspectButton.addEventListener('click', function () {
      setInspectEnabled(!inspectEnabled);
    });
  }
  if (stepPrevButton) {
    stepPrevButton.addEventListener('click', function () {
      currentStepIndex = Math.max(0, currentStepIndex - 1);
      renderStepDebugger();
    });
  }
  if (stepNextButton) {
    stepNextButton.addEventListener('click', function () {
      currentStepIndex = Math.min(Math.max(0, vmSteps.length - 1), currentStepIndex + 1);
      renderStepDebugger();
    });
  }
  if (stepRange) {
    stepRange.addEventListener('input', function () {
      var next = Number(stepRange.value) | 0;
      currentStepIndex = next;
      renderStepDebugger();
    });
  }
  renderStepDebugger();
  if ((text(trace.debugMode) === 'step' || text(vmStepDebug && vmStepDebug.mode) === 'step') && vmSteps.length > 0) {
    panel.style.display = 'block';
  }
  setInspectEnabled(false);

  document.addEventListener('mousemove', handleInspectMove, true);
  document.addEventListener('click', handleInspectClick, true);
})();
</script>`;
}
