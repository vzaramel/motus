/**
 * Development render trace helpers for edge responses.
 */

function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

export function isDebugEnabled(env) {
  const raw = String(env?.MOT_DEBUG ?? '').trim().toLowerCase();
  return raw === '1' || raw === 'true' || raw === 'yes' || raw === 'on';
}

export class RenderTrace {
  constructor({ page, vmMode, originUrl, debugMode = '' }) {
    this.startedAt = nowMs();
    this.page = page;
    this.vmMode = vmMode;
    this.originUrl = originUrl;
    this.debugMode = debugMode ? String(debugMode) : '';
    this.vmUsed = null;
    this.bytecodeSize = 0;
    this.dependencies = [];
    this.executionPaths = [];
    this.spans = [];
    this.dataFetches = [];
    this.componentLoads = [];
    this.componentRenders = [];
    this.componentSourceMaps = {};
    this.bytecodeDebug = null;
    this.vmStepDebug = null;
    this.error = null;
  }

  startSpan(name, meta = {}) {
    return {
      name,
      startedAt: nowMs(),
      meta,
    };
  }

  endSpan(span, status = 'ok', extra = {}) {
    if (!span || !span.name) return;
    const endedAt = nowMs();
    this.spans.push({
      name: span.name,
      status,
      durationMs: endedAt - span.startedAt,
      ...span.meta,
      ...extra,
    });
  }

  recordDataFetch(event) {
    this.dataFetches.push({
      queryRef: Number(event?.queryRef ?? 0) >>> 0,
      signature: Number(event?.signature ?? 0) >>> 0,
      name: event?.name ? String(event.name) : '',
      status: event?.status ?? 'ok',
      durationMs: Number(event?.durationMs ?? 0),
      rowCount: Number(event?.rowCount ?? 0),
      page: event?.page ? String(event.page) : this.page,
      fetchMode: event?.fetchMode ? String(event.fetchMode) : 'origin_first_render',
      componentPath: event?.componentPath ? String(event.componentPath) : '',
      componentCallKind: event?.componentCallKind ? String(event.componentCallKind) : 'page',
      error: event?.error ? String(event.error) : null,
    });
  }

  recordExecutionPath(path) {
    const normalized = path ? String(path) : '';
    if (!normalized) return;
    if (!this.executionPaths.includes(normalized)) {
      this.executionPaths.push(normalized);
    }
  }

  recordComponentLoad(event) {
    this.componentLoads.push({
      path: event?.path ? String(event.path) : '',
      source: event?.source ? String(event.source) : 'origin',
      artifactSource: event?.artifactSource ? String(event.artifactSource) : null,
      status: event?.status ?? 'ok',
      durationMs: Number(event?.durationMs ?? 0),
      sizeBytes: Number(event?.sizeBytes ?? 0),
      error: event?.error ? String(event.error) : null,
    });
  }

  recordComponentRender(event) {
    this.componentRenders.push({
      path: event?.path ? String(event.path) : '',
      phase: event?.phase ? String(event.phase) : 'end',
      status: event?.status ?? 'ok',
      durationMs: Number(event?.durationMs ?? 0),
      renderEngine: event?.renderEngine ? String(event.renderEngine) : 'unknown',
      callKind: event?.callKind ? String(event.callKind) : 'unknown',
      componentOpcode: event?.componentOpcode ? String(event.componentOpcode) : null,
      loadSource: event?.loadSource ? String(event.loadSource) : 'unknown',
      artifactSource: event?.artifactSource ? String(event.artifactSource) : null,
      sizeBytes: Number(event?.sizeBytes ?? 0),
      error: event?.error ? String(event.error) : null,
    });
  }

  recordComponentSourceMap(event) {
    const path = event?.path ? String(event.path) : '';
    const sourceMap = event?.sourceMap && typeof event.sourceMap === 'object' ? event.sourceMap : null;
    const source = event?.source ? String(event.source) : 'unknown';
    if (!path || !sourceMap) return;

    if (!Object.prototype.hasOwnProperty.call(this.componentSourceMaps, path) &&
        Object.keys(this.componentSourceMaps).length >= 256) {
      return;
    }

    this.componentSourceMaps[path] = {
      source,
      sourceMap,
    };
  }

  setResult({ vmUsed, bytecodeSize, dependencies }) {
    this.vmUsed = vmUsed ? String(vmUsed) : this.vmUsed;
    this.bytecodeSize = Number(bytecodeSize ?? this.bytecodeSize ?? 0);
    this.dependencies = Array.isArray(dependencies) ? dependencies.slice(0, 2048) : this.dependencies;
  }

  setDebugMetadata(meta) {
    if (!meta || typeof meta !== 'object') {
      this.bytecodeDebug = null;
      return;
    }
    this.bytecodeDebug = {
      spans: Array.isArray(meta.spans) ? meta.spans.slice(0, 5000) : [],
      queries: Array.isArray(meta.queries) ? meta.queries.slice(0, 500) : [],
      componentRefs: Array.isArray(meta.componentRefs) ? meta.componentRefs.slice(0, 500) : [],
      functionComponents: Array.isArray(meta.functionComponents) ? meta.functionComponents.slice(0, 1000) : [],
      functionChunkSizes: Array.isArray(meta.functionChunkSizes) ? meta.functionChunkSizes.slice(0, 1000) : [],
      mainChunkSize: Number(meta.mainChunkSize ?? 0),
      dataRequirements: Array.isArray(meta.dataRequirements) ? meta.dataRequirements.slice(0, 500) : [],
      sourceMap: meta.sourceMap && typeof meta.sourceMap === 'object' ? meta.sourceMap : null,
      error: meta.error ? String(meta.error) : null,
    };
  }

  setVmStepDebug(meta) {
    if (!meta || typeof meta !== 'object') {
      this.vmStepDebug = null;
      return;
    }
    const rawSteps = Array.isArray(meta.steps) ? meta.steps.slice(0, 5000) : [];
    const rawSources = meta.sources && typeof meta.sources === 'object' ? meta.sources : {};
    const sources = {};
    let sourceCount = 0;
    for (const [key, value] of Object.entries(rawSources)) {
      if (!key || sourceCount >= 128) break;
      sources[String(key)] = String(value ?? '').slice(0, 200000);
      sourceCount += 1;
    }

    this.vmStepDebug = {
      mode: meta.mode ? String(meta.mode) : 'step',
      enabled: meta.enabled === true,
      truncated: meta.truncated === true,
      totalSteps: Number(meta.totalSteps ?? rawSteps.length),
      maxSteps: Number(meta.maxSteps ?? rawSteps.length),
      steps: rawSteps,
      sources,
    };
  }

  setError(error) {
    this.error = error ? String(error) : null;
  }

  toJSON() {
    return {
      page: this.page,
      vmMode: this.vmMode,
      debugMode: this.debugMode,
      vmUsed: this.vmUsed,
      originUrl: this.originUrl,
      bytecodeSize: this.bytecodeSize,
      totalDurationMs: nowMs() - this.startedAt,
      dependencies: this.dependencies,
      executionPaths: this.executionPaths,
      spans: this.spans,
      dataFetches: this.dataFetches,
      componentLoads: this.componentLoads,
      componentRenders: this.componentRenders,
      componentSourceMaps: this.componentSourceMaps,
      bytecodeDebug: this.bytecodeDebug,
      vmStepDebug: this.vmStepDebug,
      error: this.error,
    };
  }
}

export function renderTraceScriptTag(trace) {
  const payload = trace && typeof trace.toJSON === 'function' ? trace.toJSON() : trace;
  const json = JSON.stringify(payload ?? {});
  const safeJson = json.replace(/<\/script/gi, '<\\/script');
  return `<script id="mot-trace-data" type="application/json">${safeJson}</script>`;
}
