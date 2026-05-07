import assert from 'node:assert/strict';
import { renderDevtoolsBootstrapScript } from './src/devtools-inline.js';

function run() {
  const script = renderDevtoolsBootstrapScript();
  assert.ok(script.startsWith('<script>'));
  assert.ok(script.includes('mot-debug-toggle'));
  assert.ok(script.includes('mot-debug-panel'));
  assert.ok(script.includes('mot-debug-inspect-toggle'));
  assert.ok(script.includes('mot-debug-outline'));
  assert.ok(script.includes('data-mot-source'));
  assert.ok(script.includes('Motus Debug'));
  assert.ok(script.includes('Source Map'));
  assert.ok(script.includes('Component Maps'));
  assert.ok(script.includes('Component Renders'));
  assert.ok(script.includes('Step Debugger'));
  assert.ok(script.includes('mot-step-prev'));
  assert.ok(script.includes('mot-step-source'));
  assert.ok(script.includes('op='));
  console.log('devtools-inline tests passed');
}

run();
