import assert from 'node:assert/strict';
import { buildBytecodeDebugSourceMap, extractBytecodeDebugMetadata } from './src/bytecode-debug.js';

const BYTECODE_MAGIC = 0x00544F4D;
const VERSION_MAJOR = 1;
const VERSION_MINOR = 4;
const BYTECODE_DEBUG_MAGIC = 0x4742444D;
const BC_HALT = 63;

class Writer {
  constructor() {
    this.bytes = [];
  }
  u8(v) { this.bytes.push(v & 0xff); }
  u16(v) { this.u8(v); this.u8(v >>> 8); }
  u32(v) { this.u8(v); this.u8(v >>> 8); this.u8(v >>> 16); this.u8(v >>> 24); }
  bytesRaw(arr) { for (const b of arr) this.u8(b); }
  str(s) {
    const enc = new TextEncoder().encode(s);
    this.u32(enc.length);
    this.bytesRaw(enc);
  }
  out() { return new Uint8Array(this.bytes); }
}

function buildMinimalBytecode({ withDebug }) {
  const w = new Writer();
  w.u32(BYTECODE_MAGIC);
  w.u16(VERSION_MAJOR);
  w.u16(VERSION_MINOR);
  w.u16(0); // flags

  // counts: const, string, dataReq, dep, builtin, compRef, func
  w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0); w.u32(0);

  // main chunk
  w.u32(1);
  w.u8(BC_HALT);

  if (withDebug) {
    w.u32(BYTECODE_DEBUG_MAGIC);
    w.u32(1); // span count
    w.u8(0); // chunk kind main
    w.u16(0); // chunk index
    w.u32(0); // start pc
    w.u32(1); // end pc
    w.u32(12); // line
    w.u32(3); // column
    w.u16(0); // node type

    w.u32(1); // query count
    w.u32(7); // queryRef
    w.u32(11); // signature
    w.u32(21); // line
    w.u32(9); // column
    const name = new TextEncoder().encode('products');
    w.u16(name.length);
    w.bytesRaw(name);
  }

  return w.out();
}

function run() {
  const noDebug = extractBytecodeDebugMetadata(buildMinimalBytecode({ withDebug: false }));
  assert.equal(noDebug.error, null);
  assert.equal(noDebug.spans.length, 0);
  assert.equal(noDebug.queries.length, 0);

  const withDebug = extractBytecodeDebugMetadata(buildMinimalBytecode({ withDebug: true }));
  assert.equal(withDebug.error, null);
  assert.equal(withDebug.spans.length, 1);
  assert.equal(withDebug.spans[0].line, 12);
  assert.equal(withDebug.queries.length, 1);
  assert.equal(withDebug.queries[0].queryRef, 7);
  assert.equal(withDebug.queries[0].signature, 11);
  assert.equal(withDebug.queries[0].name, 'products');

  const sourceMap = buildBytecodeDebugSourceMap(withDebug, {
    file: 'index.wasm',
    source: 'pages/index.mot',
  });
  assert.equal(sourceMap.version, 3);
  assert.equal(sourceMap.file, 'index.wasm');
  assert.deepEqual(sourceMap.sources, ['pages/index.mot']);
  assert.equal(typeof sourceMap.mappings, 'string');
  assert.ok(sourceMap.mappings.length > 0);

  const noMap = buildBytecodeDebugSourceMap(noDebug, {
    file: 'none.wasm',
    source: 'pages/none.mot',
  });
  assert.equal(noMap, null);

  console.log('bytecode-debug tests passed');
}

run();
