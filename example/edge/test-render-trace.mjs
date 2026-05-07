import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { RenderTrace, isDebugEnabled, renderTraceScriptTag } from './src/render-trace.js';

function normalizeForSnapshot(value) {
  if (typeof value === 'number') return 0;
  if (typeof value === 'string') return 's';
  if (Array.isArray(value)) return value.map((item) => normalizeForSnapshot(item));
  if (!value || typeof value !== 'object') return value;

  const out = {};
  for (const key of Object.keys(value)) {
    out[key] = normalizeForSnapshot(value[key]);
  }
  return out;
}

function run() {
  assert.equal(isDebugEnabled({ MOT_DEBUG: '1' }), true);
  assert.equal(isDebugEnabled({ MOT_DEBUG: 'true' }), true);
  assert.equal(isDebugEnabled({ MOT_DEBUG: 'on' }), true);
  assert.equal(isDebugEnabled({ MOT_DEBUG: '0' }), false);

  const trace = new RenderTrace({
    page: 'index',
    vmMode: 'wasm',
    originUrl: 'http://localhost:8091',
    debugMode: 'step',
  });

  const span = trace.startSpan('compile');
  trace.endSpan(span, 'ok', { bytes: 1234 });
  trace.recordDataFetch({
    queryRef: -1,
    signature: -2,
    durationMs: 5.5,
    rowCount: 2,
    status: 'ok',
  });
  trace.recordComponentLoad({
    path: 'components/Header',
    source: 'embedded',
    artifactSource: 'embedded',
    status: 'ok',
    durationMs: 0.1,
    sizeBytes: 512,
  });
  trace.recordComponentRender({
    path: 'components/Header',
    status: 'ok',
    durationMs: 0.4,
    renderEngine: 'wasm',
    loadSource: 'embedded',
    artifactSource: 'embedded',
  });
  trace.recordComponentSourceMap({
    path: 'components/Header',
    source: 'embedded',
    sourceMap: { version: 3, file: 'Header.wasm', sources: ['components/Header.mot'], names: [], mappings: 'AAAA' },
  });
  trace.setResult({ vmUsed: 'wasm', bytecodeSize: 999, dependencies: ['products.*'] });
  trace.recordExecutionPath('vm:wasm');
  trace.recordExecutionPath('component:wasm');
  trace.recordExecutionPath('vm:wasm');
  trace.setDebugMetadata({
    spans: [{ line: 12 }],
    queries: [{ queryRef: 1, line: 20 }],
    componentRefs: [{ path: 'components/Header' }],
    functionComponents: [{ chunkKind: 1, chunkIndex: 0, name: 'Header', sourcePath: 'components/Header.mot' }],
    dataRequirements: [{ name: 'products' }],
    sourceMap: { version: 3, file: 'index.wasm', sources: ['pages/index.mot'], names: [], mappings: 'AAAA' },
  });
  trace.setVmStepDebug({
    mode: 'step',
    enabled: true,
    truncated: false,
    totalSteps: 2,
    maxSteps: 100,
    steps: [
      { opcode: 36, opcodeName: 'BC_EMIT_LITERAL', line: 4, sourcePath: 'pages/index.mot' },
    ],
    sources: {
      'pages/index.mot': '<Layout />',
    },
  });

  const json = trace.toJSON();
  assert.equal(json.page, 'index');
  assert.equal(json.vmMode, 'wasm');
  assert.equal(json.debugMode, 'step');
  assert.equal(json.vmUsed, 'wasm');
  assert.equal(json.bytecodeSize, 999);
  assert.deepEqual(json.dependencies, ['products.*']);
  assert.deepEqual(json.executionPaths, ['vm:wasm', 'component:wasm']);
  assert.equal(json.dataFetches[0].queryRef, 0xFFFFFFFF);
  assert.equal(json.dataFetches[0].signature, 0xFFFFFFFE);
  assert.equal(json.dataFetches[0].fetchMode, 'origin_first_render');
  assert.equal(json.dataFetches[0].componentCallKind, 'page');
  assert.equal(json.componentLoads[0].path, 'components/Header');
  assert.equal(json.componentLoads[0].artifactSource, 'embedded');
  assert.equal(json.componentRenders[0].renderEngine, 'wasm');
  assert.equal(json.componentRenders[0].loadSource, 'embedded');
  assert.equal(json.componentSourceMaps['components/Header'].sourceMap.file, 'Header.wasm');
  assert.equal(json.spans[0].name, 'compile');
  assert.equal(json.bytecodeDebug.spans.length, 1);
  assert.equal(json.bytecodeDebug.queries.length, 1);
  assert.equal(json.bytecodeDebug.functionComponents.length, 1);
  assert.equal(json.bytecodeDebug.sourceMap.file, 'index.wasm');
  assert.equal(json.vmStepDebug.mode, 'step');
  assert.equal(json.vmStepDebug.steps.length, 1);
  assert.equal(json.vmStepDebug.sources['pages/index.mot'], '<Layout />');

  const snapshotExpected = JSON.parse(
    readFileSync(new URL('./render-trace.snapshot.json', import.meta.url), 'utf8'),
  );
  assert.deepEqual(
    normalizeForSnapshot(json),
    snapshotExpected,
    'Render trace schema snapshot mismatch',
  );

  const tag = renderTraceScriptTag({
    toJSON: () => ({ a: '</script><script>alert(1)</script>' }),
  });
  assert.match(tag, /id="mot-trace-data"/);
  assert.ok(tag.includes('<\\/script>'));

  console.log('render-trace tests passed');
}

run();
