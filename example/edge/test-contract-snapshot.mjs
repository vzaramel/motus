import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import * as edgeContract from './src/runtime-contract.js';
import * as wasmContract from '../../runtime-wasm/src/runtime-contract.js';

function buildSnapshot(mod) {
  return {
    normalizeU32: mod.normalizeU32(-1),
    normalizeDataRequirement: mod.normalizeDataRequirement({
      queryRef: -2,
      signature: -3,
      isSingle: 1,
      name: 'products',
    }),
    buildDataRpcPayload: mod.buildDataRpcPayload(
      { queryRef: -1, signature: 123, isSingle: false },
      { category: 'audio', page: 2 },
    ),
    buildDataRpcUrl: mod.buildDataRpcUrl('http://localhost:8091', 'products/list'),
    normalizeComponentPath: mod.normalizeComponentPath('components/ui/Card'),
    normalizeComponentRef: mod.normalizeComponentRef({ path: 'components/ui/Card' }),
    componentRouteName: mod.componentRouteName('components/ui/Card'),
    buildComponentRpcUrl: mod.buildComponentRpcUrl('http://localhost:8091', 'components/ui/Card'),
  };
}

function run() {
  const expected = JSON.parse(readFileSync(new URL('./contract-snapshot.json', import.meta.url), 'utf8'));
  const edge = buildSnapshot(edgeContract);
  const wasm = buildSnapshot(wasmContract);

  assert.deepEqual(edge, expected, 'Edge contract helpers diverged from snapshot');
  assert.deepEqual(wasm, expected, 'WASM contract helpers diverged from snapshot');
  assert.deepEqual(edge, wasm, 'Edge and WASM contract helper outputs differ');

  console.log('runtime-contract snapshot tests passed');
}

run();
