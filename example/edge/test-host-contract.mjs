import assert from 'node:assert/strict';
import {
  buildComponentRpcUrl,
  buildDataRpcPayload,
  buildDataRpcUrl,
  componentRouteName,
  normalizeComponentPath,
  normalizeComponentRef,
  normalizeDataRequirement,
  normalizeU32,
} from './src/runtime-contract.js';

function run() {
  assert.equal(normalizeU32(-1), 0xFFFFFFFF);
  assert.equal(normalizeU32(5), 5);

  const req = normalizeDataRequirement({
    queryRef: -2,
    signature: -3,
    isSingle: 1,
    name: 'products',
  });
  assert.equal(req.queryRef, 0xFFFFFFFE);
  assert.equal(req.signature, 0xFFFFFFFD);
  assert.equal(req.isSingle, true);
  assert.equal(req.name, 'products');

  const payload = buildDataRpcPayload(
    { queryRef: -1, signature: 123, isSingle: false },
    { category: 'audio' },
  );
  assert.deepEqual(payload, {
    queryRef: 0xFFFFFFFF,
    signature: 123,
    params: { category: 'audio' },
    single: false,
  });

  assert.equal(
    buildDataRpcUrl('http://localhost:8091', 'products/list'),
    'http://localhost:8091/data/products%2Flist',
  );

  assert.equal(normalizeComponentPath('Header'), 'components/Header');
  assert.equal(normalizeComponentPath('components/ui/Card'), 'components/ui/Card');
  assert.deepEqual(normalizeComponentRef({ name: 'Header', path: 'components/Header' }), {
    name: 'Header',
    path: 'components/Header',
  });
  assert.equal(componentRouteName('components/ui/Card'), 'ui/Card');
  assert.equal(
    buildComponentRpcUrl('http://localhost:8091', 'components/ui/Card'),
    'http://localhost:8091/component/ui%2FCard',
  );
  assert.throws(() => normalizeComponentPath('../Header'));

  console.log('runtime-contract tests passed');
}

run();
