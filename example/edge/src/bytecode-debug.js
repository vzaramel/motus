const BYTECODE_MAGIC = 0x00544F4D;
const BYTECODE_VERSION_MAJOR = 1;
const BYTECODE_VERSION_MINOR = 2;
const BYTECODE_DEBUG_MAGIC = 0x4742444D;
const BASE64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function failSafeResult(error = null) {
  return {
    spans: [],
    queries: [],
    componentRefs: [],
    linkedComponentRefs: [],
    functionComponents: [],
    functionChunkSizes: [],
    mainChunkSize: 0,
    dataRequirements: [],
    error: error ? String(error) : null,
  };
}

const LINKED_REF_MARKER_PREFIX = '@linked_ref:';

function parseLinkedRefMarker(dep) {
  const text = String(dep || '');
  if (!text.startsWith(LINKED_REF_MARKER_PREFIX)) return null;
  const body = text.slice(LINKED_REF_MARKER_PREFIX.length);
  const parts = body.split(':');
  if (parts.length !== 2) return null;
  const refIndex = Number(parts[0]);
  const funcIndex = Number(parts[1]);
  if (!Number.isInteger(refIndex) || refIndex < 0) return null;
  if (!Number.isInteger(funcIndex) || funcIndex < 0) return null;
  return { refIndex, funcIndex };
}

function encodeVlq(value) {
  let n = Math.trunc(Number(value) || 0);
  let vlq = n < 0 ? ((-n) * 2 + 1) : (n * 2);
  let out = '';
  do {
    let digit = vlq % 32;
    vlq = Math.floor(vlq / 32);
    if (vlq > 0) digit |= 32;
    out += BASE64_CHARS[digit];
  } while (vlq > 0);
  return out;
}

export function buildBytecodeDebugSourceMap(meta, options = {}) {
  const spans = Array.isArray(meta?.spans) ? meta.spans : [];
  if (spans.length === 0) return null;

  const file = String(options.file || 'module.wasm');
  const source = String(options.source || 'module.mot');

  const chunkKeyToLine = new Map();
  const grouped = new Map();

  for (const raw of spans) {
    const chunkKind = Number(raw?.chunkKind ?? 0) >>> 0;
    const chunkIndex = Number(raw?.chunkIndex ?? 0) >>> 0;
    const startPc = Number(raw?.startPc ?? 0) >>> 0;
    const sourceLine = Math.max(0, (Number(raw?.line ?? 1) || 1) - 1);
    const sourceCol = Math.max(0, (Number(raw?.column ?? 1) || 1) - 1);
    const chunkKey = `${chunkKind}:${chunkIndex}`;

    let generatedLine = chunkKeyToLine.get(chunkKey);
    if (generatedLine == null) {
      generatedLine = chunkKeyToLine.size;
      chunkKeyToLine.set(chunkKey, generatedLine);
    }

    if (!grouped.has(generatedLine)) grouped.set(generatedLine, []);
    grouped.get(generatedLine).push({
      generatedCol: startPc,
      sourceLine,
      sourceCol,
    });
  }

  const maxGeneratedLine = Math.max(...grouped.keys());
  let mappings = '';
  let prevGeneratedLine = 0;
  let prevGeneratedCol = 0;
  let prevSource = 0;
  let prevSourceLine = 0;
  let prevSourceCol = 0;

  for (let line = 0; line <= maxGeneratedLine; line++) {
    while (prevGeneratedLine < line) {
      mappings += ';';
      prevGeneratedLine += 1;
      prevGeneratedCol = 0;
    }

    const entries = grouped.get(line) || [];
    entries.sort((a, b) => a.generatedCol - b.generatedCol || a.sourceLine - b.sourceLine || a.sourceCol - b.sourceCol);

    for (let i = 0; i < entries.length; i++) {
      const entry = entries[i];
      if (i > 0) mappings += ',';
      mappings += encodeVlq(entry.generatedCol - prevGeneratedCol);
      mappings += encodeVlq(0 - prevSource); // single source: index 0
      mappings += encodeVlq(entry.sourceLine - prevSourceLine);
      mappings += encodeVlq(entry.sourceCol - prevSourceCol);
      prevGeneratedCol = entry.generatedCol;
      prevSource = 0;
      prevSourceLine = entry.sourceLine;
      prevSourceCol = entry.sourceCol;
    }
  }

  return {
    version: 3,
    file,
    sources: [source],
    names: [],
    mappings,
  };
}

export function extractBytecodeDebugMetadata(bytecode) {
  try {
    const bytes = bytecode instanceof Uint8Array ? bytecode : new Uint8Array(bytecode);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let ip = 0;

    const ensure = (n) => {
      if (ip + n > bytes.byteLength) {
        throw new Error('truncated bytecode');
      }
    };
    const readU8 = () => { ensure(1); return bytes[ip++]; };
    const readU16 = () => { ensure(2); const v = view.getUint16(ip, true); ip += 2; return v; };
    const readU32 = () => { ensure(4); const v = view.getUint32(ip, true); ip += 4; return v; };
    const readString = () => {
      const len = readU32();
      ensure(len);
      const s = new TextDecoder().decode(bytes.subarray(ip, ip + len));
      ip += len;
      return s;
    };
    const skip = (n) => { ensure(n); ip += n; };

    const magic = readU32();
    const major = readU16();
    const minor = readU16();
    skip(2); // flags

    if (magic !== BYTECODE_MAGIC || major !== BYTECODE_VERSION_MAJOR || minor !== BYTECODE_VERSION_MINOR) {
      return failSafeResult('unsupported bytecode header');
    }

    const constCount = readU32();
    const stringCount = readU32();
    const dataReqCount = readU32();
    const depCount = readU32();
    const builtinCount = readU32();
    const compRefCount = readU32();
    const funcCount = readU32();

    for (let i = 0; i < constCount; i++) {
      const type = readU8();
      if (type === 0 || type === 1) {
        skip(1);
      } else if (type === 2 || type === 3) {
        skip(8);
      } else if (type === 4) {
        const len = readU32();
        skip(len);
      } else {
        throw new Error(`invalid const type: ${type}`);
      }
    }

    for (let i = 0; i < stringCount; i++) {
      const len = readU32();
      skip(len);
    }

    const dataRequirements = [];
    for (let i = 0; i < dataReqCount; i++) {
      const name = readString();
      const isSingle = readU8() !== 0;
      const isDynamic = readU8() !== 0;
      const queryRef = readU32();
      const signature = readU32();
      const paramCount = readU16();
      const params = [];
      for (let p = 0; p < paramCount; p++) {
        const paramName = readString();
        const slot = readU16();
        params.push({ name: paramName, slot });
      }
      dataRequirements.push({ name, isSingle, isDynamic, queryRef, signature, params });
    }

    const linkedMarkers = [];
    for (let i = 0; i < depCount; i++) {
      const depPath = readString();
      const marker = parseLinkedRefMarker(depPath);
      if (marker) linkedMarkers.push(marker);
    }

    for (let i = 0; i < builtinCount; i++) {
      const len = readU32();
      skip(len);
      skip(2); // min/max args
    }

    const componentRefs = [];
    for (let i = 0; i < compRefCount; i++) {
      const name = readString();
      const path = readString();
      componentRefs.push({ name, path });
    }

    const linkedComponentRefs = linkedMarkers
      .filter((m) => m.refIndex >= 0 && m.refIndex < componentRefs.length)
      .map((m) => ({
        refIndex: m.refIndex,
        funcIndex: m.funcIndex,
        name: componentRefs[m.refIndex].name,
        path: componentRefs[m.refIndex].path,
      }));

    const mainLen = readU32();
    skip(mainLen);
    const functionChunkSizes = [];
    for (let i = 0; i < funcCount; i++) {
      const fnLen = readU32();
      functionChunkSizes.push({
        chunkKind: 1,
        chunkIndex: i,
        sizeBytes: fnLen,
      });
      skip(fnLen);
    }

    const result = {
      spans: [],
      queries: [],
      componentRefs,
      linkedComponentRefs,
      functionComponents: [],
      functionChunkSizes,
      mainChunkSize: mainLen,
      dataRequirements,
      error: null,
    };

    if (ip >= bytes.byteLength) {
      return result;
    }

    const dbgMagic = readU32();
    if (dbgMagic !== BYTECODE_DEBUG_MAGIC) {
      return result;
    }

    const spanCount = readU32();
    for (let i = 0; i < spanCount; i++) {
      const chunkKind = readU8();
      const chunkIndex = readU16();
      const startPc = readU32();
      const endPc = readU32();
      const line = readU32();
      const column = readU32();
      const nodeType = readU16();
      result.spans.push({
        chunkKind,
        chunkIndex,
        startPc,
        endPc,
        line,
        column,
        nodeType,
      });
    }

    const queryCount = readU32();
    for (let i = 0; i < queryCount; i++) {
      const queryRef = readU32();
      const signature = readU32();
      const line = readU32();
      const column = readU32();
      const nameLen = readU16();
      ensure(nameLen);
      const name = new TextDecoder().decode(bytes.subarray(ip, ip + nameLen));
      ip += nameLen;
      result.queries.push({ queryRef, signature, line, column, name });
    }

    if (ip < bytes.byteLength) {
      const funcComponentCount = readU32();
      for (let i = 0; i < funcComponentCount; i++) {
        const chunkIndex = readU16();
        const nameLen = readU16();
        ensure(nameLen);
        const name = new TextDecoder().decode(bytes.subarray(ip, ip + nameLen));
        ip += nameLen;
        const sourceLen = readU16();
        ensure(sourceLen);
        const sourcePath = new TextDecoder().decode(bytes.subarray(ip, ip + sourceLen));
        ip += sourceLen;
        result.functionComponents.push({
          chunkKind: 1,
          chunkIndex,
          name,
          sourcePath,
        });
      }
    }

    return result;
  } catch (err) {
    return failSafeResult(err?.message || String(err));
  }
}
