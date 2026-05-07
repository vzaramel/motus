import assert from 'node:assert/strict';
import {
  renderBrowserWasmBootstrapScript,
  shouldEnableBrowserWasm,
} from './src/browser-wasm-inline.js';

function testShouldEnableFromEnv() {
  const url = new URL('https://example.test/index');
  assert.equal(shouldEnableBrowserWasm({ MOT_BROWSER_WASM: '1' }, url), true);
  assert.equal(shouldEnableBrowserWasm({ MOT_BROWSER_WASM: 'true' }, url), true);
  assert.equal(shouldEnableBrowserWasm({ MOT_BROWSER_WASM: '0' }, url), false);
}

function testShouldEnableFromQueryParam() {
  const on = new URL('https://example.test/index?browser_wasm=1');
  const off = new URL('https://example.test/index?browser_wasm=0');
  assert.equal(shouldEnableBrowserWasm({}, on), true);
  assert.equal(shouldEnableBrowserWasm({}, off), false);
}

function testBootstrapScriptShape() {
  const script = renderBrowserWasmBootstrapScript({
    rpcBasePath: '/_mot',
    pageName: 'index',
  });

  assert.match(script, /id="mot-browser-wasm-bootstrap"/);
  assert.match(script, /window\.__motBrowserWasmBootstrapped/);
  assert.match(script, /rpcBasePath\s*=\s*"\/_mot"/);
  assert.match(script, /buildRuntimeWasmRpcUrl\(/);
  assert.match(script, /normalizeRpcBase\(rpcBasePath\) \+ '\/runtime\/wasm'/);
  assert.match(script, /normalizeRpcBase\(rpcBasePath\) \+ '\/page\//);
  assert.match(script, /normalizeRpcBase\(rpcBasePath\) \+ '\/component\//);
  assert.match(script, /normalizeRpcBase\(rpcBasePath\) \+ '\/source\//);
  assert.match(script, /\/page\/'\s*\+\s*encodeURIComponent\(page\)/);
  assert.match(script, /applyRenderedHtmlInPlace\(html\)/);
  assert.match(script, /morphChildList\(document\.body, parsed\.body\)/);
  assert.match(script, /window\.motSetVar\s*=\s*function/);
  assert.match(script, /window\.motFlushReactivity\s*=\s*function/);
  assert.match(script, /window\.motCreateState\s*=\s*function/);
  assert.match(script, /window\.motSignal\s*=\s*function/);
  assert.match(script, /reactiveWasmPayload/);
  assert.match(script, /initReactiveWasmModule\(/);
  assert.match(script, /evaluateReactiveExpr\(/);
  assert.match(script, /window\.__motReactiveWasm/);
  assert.match(script, /installAutoReactiveActions\(/);
  assert.match(script, /window\.motInstallActions\s*=\s*function/);
  assert.match(script, /data-mot-inc/);
  assert.match(script, /data-mot-set-path/);
  assert.match(script, /data-mot-bind/);
  assert.match(script, /@bind:/);
  assert.match(script, /@set:/);
  assert.match(script, /@plan:/);
  assert.match(script, /@bindattr:/);
  assert.match(script, /@planattr:/);
  assert.match(script, /@exprbind:/);
  assert.match(script, /@exprattr:/);
  assert.match(script, /@exprdep:/);
  assert.match(script, /@planexpr:/);
  assert.match(script, /extractDependencyMarkersFromBytecode/);
  assert.match(script, /registerReactiveMetadata\(bytecode\)/);
  assert.match(script, /parseSyntheticPlan/);
  assert.match(script, /parseSyntheticBindAttr/);
  assert.match(script, /parseSyntheticPlanAttr/);
  assert.match(script, /parseSyntheticExprBind/);
  assert.match(script, /parseSyntheticExprAttr/);
  assert.match(script, /parseSyntheticExprDep/);
  assert.match(script, /parseSyntheticPlanExpr/);
  assert.match(script, /planSetToBind/);
  assert.match(script, /planAttrBySet/);
  assert.match(script, /planExprBySet/);
  assert.match(script, /selectCandidateBindings\(normalizedName\)/);
  assert.match(script, /selectCandidateAttrRules\(normalizedName\)/);
  assert.match(script, /selectCandidateExprIds\(normalizedName\)/);
  assert.match(script, /enqueueMutation\(/);
  assert.match(script, /flushMutationQueue\(/);
  assert.match(script, /scheduleMutationFlush\(/);
  assert.match(script, /pageBytecodeCache/);
  assert.match(script, /componentBytecodeCache/);
  assert.match(script, /evaluateReactiveProgram\(/);
  assert.match(script, /setReactiveStateForMutation\(/);
  assert.match(script, /rebuildExprNodeIndex\(/);
  assert.match(script, /valueForBindPath\(/);
  assert.match(script, /window\.__motReactiveMeta/);
  assert.match(script, /host_debug_step/);
  assert.match(script, /mot_set_debug_step_mode/);
  assert.match(script, /createStepLiveDebuggerUi/);
  assert.match(script, /window\.__motLiveStepController/);
  assert.match(script, /Step Into/);
  assert.match(script, /data-act="bp-add"/);
  assert.match(script, /data-act="bp-del"/);
  assert.match(script, /breakpoints: none/);
  assert.match(script, /mot_resume_step/);
  assert.match(script, /mot_debug_snapshot_ptr/);
  assert.match(script, /addBreakpoint:\s*function/);
  assert.match(script, /removeBreakpoint:\s*function/);
  assert.match(script, /if \(pauseOnDebugStep && !interactiveStepMode\)/);
  assert.match(script, /pauseOnDebugStep/);
  assert.match(script, /window\.__motLiveStepLast/);
  assert.doesNotMatch(script, /document\.write\(/);
}

function run() {
  testShouldEnableFromEnv();
  testShouldEnableFromQueryParam();
  testBootstrapScriptShape();
  console.log('browser-wasm-inline tests passed');
}

run();
