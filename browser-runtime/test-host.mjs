import assert from 'node:assert/strict';
import { createBrowserHost } from './src/host.js';

function mockJsonResponse(data, status = 200) {
  return {
    ok: status >= 200 && status < 300,
    status,
    json: async () => data,
    text: async () => JSON.stringify(data),
    arrayBuffer: async () => new Uint8Array(0).buffer,
  };
}

function mockBytecodeResponse(bytes, status = 200) {
  return {
    ok: status >= 200 && status < 300,
    status,
    json: async () => ({}),
    text: async () => '',
    arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  };
}

async function testFetchDataShape() {
  const calls = [];
  const host = createBrowserHost({
    originUrl: 'http://localhost:8091',
    pageName: 'index',
    fetchImpl: async (url, init) => {
      calls.push({ url, init });
      return mockJsonResponse([{ id: 1, name: 'A' }]);
    },
  });

  const data = await host.fetchData(
    { queryRef: -1, signature: 123, isSingle: false },
    { category: 'audio' },
  );

  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'http://localhost:8091/data/index');
  assert.equal(calls[0].init.method, 'POST');
  assert.equal(calls[0].init.headers['Content-Type'], 'application/json');
  assert.deepEqual(JSON.parse(calls[0].init.body), {
    queryRef: 0xFFFFFFFF,
    signature: 123,
    params: { category: 'audio' },
    single: false,
  });
  assert.deepEqual(data, [{ id: 1, name: 'A' }]);
}

async function testFetchDataSingle() {
  const host = createBrowserHost({
    originUrl: 'http://localhost:8091',
    pageName: 'index',
    fetchImpl: async () => mockJsonResponse([{ id: 7 }, { id: 8 }]),
  });

  const data = await host.fetchData(
    { queryRef: 10, signature: 20, isSingle: true },
    {},
  );
  assert.deepEqual(data, { id: 7 });
}

async function testLoadComponentBytecodePath() {
  const calls = [];
  const bytes = new Uint8Array([1, 2, 3, 4]);
  const host = createBrowserHost({
    originUrl: 'http://localhost:8091',
    pageName: 'index',
    fetchImpl: async (url) => {
      calls.push(url);
      return mockBytecodeResponse(bytes);
    },
  });

  const out = await host.loadComponentBytecode({ name: 'Card', path: 'ui/Card' });
  assert.equal(calls[0], 'http://localhost:8091/component/ui%2FCard');
  assert.deepEqual(Array.from(out), [1, 2, 3, 4]);
}

async function testLoadComponentRejectsUnsafePath() {
  const host = createBrowserHost({
    originUrl: 'http://localhost:8091',
    pageName: 'index',
    fetchImpl: async () => {
      throw new Error('fetch should not be called');
    },
  });

  await assert.rejects(() => host.loadComponentBytecode({ path: '../Card' }));
}

async function run() {
  await testFetchDataShape();
  await testFetchDataSingle();
  await testLoadComponentBytecodePath();
  await testLoadComponentRejectsUnsafePath();
  console.log('browser host tests passed');
}

run();
