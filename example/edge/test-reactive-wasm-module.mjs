import assert from 'node:assert/strict';
import { buildReactiveWasmPayloadFromDeps } from './src/reactive-wasm-module.js';

function decodeBase64ToBytes(base64) {
  return new Uint8Array(Buffer.from(String(base64 || ''), 'base64'));
}

async function testBuildReactiveWasmPayloadFromDeps() {
  const deps = [
    '@exprbind:7|P:counterValue;I:1;O:add;',
    '@exprattr:42|title|8|P:counterValue;I:2;O:mul;',
    '@exprbind:9|P:user.name;S:%21;O:add;', // unsupported (string)
  ];

  const payload = buildReactiveWasmPayloadFromDeps(deps);
  assert.ok(payload, 'payload should be built');
  assert.equal(payload.version, 1);
  assert.ok(payload.wasmBase64.length > 0, 'wasm payload should exist');
  assert.equal(payload.exprExports['7'], 'e7');
  assert.equal(payload.exprExports['8'], 'e8');
  assert.equal(payload.exprExports['9'], undefined, 'string expression should not compile to wasm');

  const pathById = payload.pathById || {};
  const counterPathId = Number(Object.keys(pathById).find((id) => pathById[id] === 'counterValue'));
  assert.ok(Number.isInteger(counterPathId), 'counterValue path id should exist');

  const bytes = decodeBase64ToBytes(payload.wasmBase64);
  const instanceResult = await WebAssembly.instantiate(bytes, {
    env: {
      host_get(pathId) {
        if (Number(pathId) === counterPathId) return 3;
        return 0;
      },
    },
  });

  const instance = instanceResult.instance || instanceResult;
  assert.equal(typeof instance.exports.e7, 'function');
  assert.equal(typeof instance.exports.e8, 'function');
  assert.equal(instance.exports.e7(), 4);
  assert.equal(instance.exports.e8(), 6);
}

async function run() {
  await testBuildReactiveWasmPayloadFromDeps();
  console.log('reactive-wasm-module tests passed');
}

run();
