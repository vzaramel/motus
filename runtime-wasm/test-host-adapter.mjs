import assert from 'node:assert/strict';
import { createHost } from './src/host.js';

const encoder = new TextEncoder();

function writeCString(memory, ptr, text) {
  const bytes = encoder.encode(text);
  const mem = new Uint8Array(memory.buffer);
  mem.set(bytes, ptr);
  mem[ptr + bytes.length] = 0;
}

async function testHostFetchDataCallback() {
  const memory = new WebAssembly.Memory({ initial: 1 });
  const calls = [];
  const host = createHost({
    writer: { write: async () => {} },
    encoder,
    fetchData: async (req, params) => {
      calls.push({ req, params });
      return { ok: true };
    },
  });
  host.setMemory(memory);

  writeCString(memory, 16, 'products');
  writeCString(memory, 64, '{"category":"audio","limit":4}');

  host.imports.env.host_fetch_data(7, -1, -2, 16, 64, 1);
  await new Promise((resolve) => setTimeout(resolve, 0));

  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0].req, {
    reqId: 7,
    queryRef: 0xFFFFFFFF,
    signature: 0xFFFFFFFE,
    name: 'products',
    isSingle: true,
  });
  assert.deepEqual(calls[0].params, { category: 'audio', limit: 4 });
}

async function run() {
  await testHostFetchDataCallback();
  console.log('runtime-wasm host adapter tests passed');
}

run();
