function readU8(view, cursor) {
  if (cursor.value + 1 > view.byteLength) throw new Error('bytecode read overflow (u8)');
  const v = view.getUint8(cursor.value);
  cursor.value += 1;
  return v;
}

function readU16(view, cursor) {
  if (cursor.value + 2 > view.byteLength) throw new Error('bytecode read overflow (u16)');
  const v = view.getUint16(cursor.value, true);
  cursor.value += 2;
  return v;
}

function readU32(view, cursor) {
  if (cursor.value + 4 > view.byteLength) throw new Error('bytecode read overflow (u32)');
  const v = view.getUint32(cursor.value, true);
  cursor.value += 4;
  return v;
}

function skip(view, cursor, len) {
  const n = Number(len) >>> 0;
  if (cursor.value + n > view.byteLength) throw new Error('bytecode skip overflow');
  cursor.value += n;
}

function readLenString(bytes, view, cursor) {
  const len = readU32(view, cursor);
  if (len === 0) return '';
  if (cursor.value + len > view.byteLength) throw new Error('bytecode string overflow');
  const out = new TextDecoder().decode(bytes.subarray(cursor.value, cursor.value + len));
  cursor.value += len;
  return out;
}

export function extractDependencyMarkersFromBytecode(bytecode) {
  try {
    if (!(bytecode instanceof Uint8Array) || bytecode.byteLength < 32) return [];
    const bytes = bytecode;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const cursor = { value: 0 };

    const magic = readU32(view, cursor);
    if (magic !== 0x00544F4D) return [];

    readU16(view, cursor);
    readU16(view, cursor);
    readU16(view, cursor);

    const constCount = readU32(view, cursor);
    const stringCount = readU32(view, cursor);
    const dataReqCount = readU32(view, cursor);
    const depCount = readU32(view, cursor);
    const builtinCount = readU32(view, cursor);
    const compRefCount = readU32(view, cursor);
    const funcCount = readU32(view, cursor);

    for (let i = 0; i < constCount; i++) {
      const type = readU8(view, cursor);
      if (type === 0 || type === 1) {
        readU8(view, cursor);
      } else if (type === 2 || type === 3) {
        skip(view, cursor, 8);
      } else if (type === 4) {
        skip(view, cursor, readU32(view, cursor));
      } else {
        return [];
      }
    }

    for (let s = 0; s < stringCount; s++) {
      skip(view, cursor, readU32(view, cursor));
    }

    for (let d = 0; d < dataReqCount; d++) {
      skip(view, cursor, readU32(view, cursor));
      skip(view, cursor, 1);
      skip(view, cursor, 1);
      skip(view, cursor, 4);
      skip(view, cursor, 4);
      const paramCount = readU16(view, cursor);
      for (let p = 0; p < paramCount; p++) {
        skip(view, cursor, readU32(view, cursor));
        skip(view, cursor, 2);
      }
    }

    const deps = [];
    for (let dep = 0; dep < depCount; dep++) {
      deps.push(readLenString(bytes, view, cursor));
    }

    for (let b = 0; b < builtinCount; b++) {
      skip(view, cursor, readU32(view, cursor));
      skip(view, cursor, 1);
      skip(view, cursor, 1);
    }

    for (let c = 0; c < compRefCount; c++) {
      skip(view, cursor, readU32(view, cursor));
      skip(view, cursor, readU32(view, cursor));
    }

    skip(view, cursor, readU32(view, cursor));
    for (let f = 0; f < funcCount; f++) {
      skip(view, cursor, readU32(view, cursor));
    }
    return deps;
  } catch (_) {
    return [];
  }
}

function parseExprBind(depPath) {
  const normalized = String(depPath || '').trim();
  if (!normalized.startsWith('@exprbind:')) return null;
  const body = normalized.slice(10);
  const sep = body.indexOf('|');
  if (sep <= 0 || sep >= body.length - 1) return null;
  const exprId = body.slice(0, sep).trim();
  const program = body.slice(sep + 1).trim();
  if (!exprId || !program) return null;
  return { exprId, program };
}

function parseExprAttr(depPath) {
  const normalized = String(depPath || '').trim();
  if (!normalized.startsWith('@exprattr:')) return null;
  const body = normalized.slice(10);
  const sep1 = body.indexOf('|');
  if (sep1 <= 0 || sep1 >= body.length - 1) return null;
  const sep2 = body.indexOf('|', sep1 + 1);
  if (sep2 <= sep1 + 1 || sep2 >= body.length - 1) return null;
  const sep3 = body.indexOf('|', sep2 + 1);
  if (sep3 <= sep2 + 1 || sep3 >= body.length - 1) return null;
  const exprId = body.slice(sep2 + 1, sep3).trim();
  const program = body.slice(sep3 + 1).trim();
  if (!exprId || !program) return null;
  return { exprId, program };
}

function decodeTokenValue(raw) {
  const text = String(raw == null ? '' : raw);
  if (!text) return '';
  try {
    return decodeURIComponent(text);
  } catch (_) {
    return text;
  }
}

function parseProgramTokens(program) {
  return String(program || '')
    .split(';')
    .map((token) => token.trim())
    .filter(Boolean);
}

function encodeU32(value) {
  let v = Number(value) >>> 0;
  const out = [];
  do {
    let byte = v & 0x7f;
    v >>>= 7;
    if (v !== 0) byte |= 0x80;
    out.push(byte);
  } while (v !== 0);
  return out;
}

function encodeI32(value) {
  let v = value | 0;
  const out = [];
  let more = true;
  while (more) {
    let byte = v & 0x7f;
    v >>= 7;
    const signBit = byte & 0x40;
    if ((v === 0 && !signBit) || (v === -1 && signBit)) {
      more = false;
    } else {
      byte |= 0x80;
    }
    out.push(byte);
  }
  return out;
}

function encodeF64(value) {
  const buf = new ArrayBuffer(8);
  new DataView(buf).setFloat64(0, Number(value), true);
  return Array.from(new Uint8Array(buf));
}

function encodeName(text) {
  const bytes = new TextEncoder().encode(String(text || ''));
  return [...encodeU32(bytes.length), ...bytes];
}

function makeSection(id, payload) {
  return [id, ...encodeU32(payload.length), ...payload];
}

function startsWithAny(token, prefixes) {
  for (let i = 0; i < prefixes.length; i++) {
    if (token.startsWith(prefixes[i])) return true;
  }
  return false;
}

function isNumericCompatible(program) {
  const tokens = parseProgramTokens(program);
  if (!tokens.length) return false;
  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i];
    if (token === 'Z' || token === 'T') continue;
    if (startsWithAny(token, ['I:', 'N:', 'B:', 'P:'])) continue;
    if (token.startsWith('U:')) {
      const op = decodeTokenValue(token.slice(2));
      if (op === 'neg' || op === 'not') continue;
      return false;
    }
    if (token.startsWith('O:')) {
      const op = decodeTokenValue(token.slice(2));
      if (
        op === 'add' || op === 'sub' || op === 'mul' || op === 'div' ||
        op === 'lt' || op === 'lte' || op === 'gt' || op === 'gte' ||
        op === 'eq' || op === 'neq' || op === 'and' || op === 'or'
      ) {
        continue;
      }
      return false;
    }
    return false;
  }
  return true;
}

function gatherPathIds(programByExprId) {
  const pathToId = Object.create(null);
  const idToPath = Object.create(null);
  let nextId = 0;
  const exprIds = Object.keys(programByExprId);
  for (let i = 0; i < exprIds.length; i++) {
    const tokens = parseProgramTokens(programByExprId[exprIds[i]]);
    for (let t = 0; t < tokens.length; t++) {
      const token = tokens[t];
      if (!token.startsWith('P:')) continue;
      const path = decodeTokenValue(token.slice(2));
      if (!path || pathToId[path] != null) continue;
      const id = nextId++;
      pathToId[path] = id;
      idToPath[String(id)] = path;
    }
  }
  return { pathToId, idToPath };
}

function emitPushPath(code, pathToId, path) {
  const id = pathToId[path];
  if (id == null) return false;
  code.push(0x41, ...encodeI32(id)); // i32.const
  code.push(0x10, ...encodeU32(0));  // call import host_get
  return true;
}

function emitToBoolI32(code) {
  code.push(0x44, ...encodeF64(0)); // f64.const 0
  code.push(0x62); // f64.ne -> i32
}

function emitI32ToF64(code) {
  code.push(0xb7); // f64.convert_i32_s
}

function compileProgramToWasmBody(program, pathToId) {
  const tokens = parseProgramTokens(program);
  const code = [];
  let stackDepth = 0;

  function pop1ToLocal(localIdx) {
    if (stackDepth < 1) return false;
    code.push(0x21, ...encodeU32(localIdx)); // local.set
    stackDepth -= 1;
    return true;
  }

  function pushF64Const(value) {
    code.push(0x44, ...encodeF64(value));
    stackDepth += 1;
  }

  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i];
    if (token === 'Z') {
      pushF64Const(0);
      continue;
    }
    if (token === 'T') {
      if (!pop1ToLocal(2) || !pop1ToLocal(1) || !pop1ToLocal(0)) return null;
      code.push(0x20, ...encodeU32(1)); // local.get then
      code.push(0x20, ...encodeU32(2)); // local.get else
      code.push(0x20, ...encodeU32(0)); // local.get cond
      emitToBoolI32(code);
      code.push(0x1b); // select
      stackDepth += 1;
      continue;
    }

    const sep = token.indexOf(':');
    const head = sep >= 0 ? token.slice(0, sep) : token;
    const tail = sep >= 0 ? token.slice(sep + 1) : '';
    const decoded = decodeTokenValue(tail);

    if (head === 'I' || head === 'N') {
      const num = Number(decoded);
      if (!Number.isFinite(num)) return null;
      pushF64Const(num);
      continue;
    }
    if (head === 'B') {
      pushF64Const(decoded === '1' ? 1 : 0);
      continue;
    }
    if (head === 'P') {
      if (!emitPushPath(code, pathToId, decoded)) return null;
      stackDepth += 1;
      continue;
    }
    if (head === 'U') {
      if (!pop1ToLocal(0)) return null;
      code.push(0x20, ...encodeU32(0)); // local.get 0
      if (decoded === 'neg') {
        code.push(0x9a); // f64.neg
      } else if (decoded === 'not') {
        emitToBoolI32(code); // i32 truthy
        code.push(0x45);     // i32.eqz
        emitI32ToF64(code);
      } else {
        return null;
      }
      stackDepth += 1;
      continue;
    }
    if (head === 'O') {
      if (!pop1ToLocal(1) || !pop1ToLocal(0)) return null;
      if (decoded === 'add' || decoded === 'sub' || decoded === 'mul' || decoded === 'div') {
        code.push(0x20, ...encodeU32(0));
        code.push(0x20, ...encodeU32(1));
        if (decoded === 'add') code.push(0xa0);
        else if (decoded === 'sub') code.push(0xa1);
        else if (decoded === 'mul') code.push(0xa2);
        else code.push(0xa3);
      } else if (
        decoded === 'lt' || decoded === 'lte' || decoded === 'gt' ||
        decoded === 'gte' || decoded === 'eq' || decoded === 'neq'
      ) {
        code.push(0x20, ...encodeU32(0));
        code.push(0x20, ...encodeU32(1));
        if (decoded === 'lt') code.push(0x63);
        else if (decoded === 'gt') code.push(0x64);
        else if (decoded === 'lte') code.push(0x65);
        else if (decoded === 'gte') code.push(0x66);
        else if (decoded === 'eq') code.push(0x61);
        else code.push(0x62);
        emitI32ToF64(code);
      } else if (decoded === 'and' || decoded === 'or') {
        code.push(0x20, ...encodeU32(0));
        emitToBoolI32(code);
        code.push(0x20, ...encodeU32(1));
        emitToBoolI32(code);
        code.push(decoded === 'and' ? 0x71 : 0x72); // i32.and / i32.or
        emitI32ToF64(code);
      } else {
        return null;
      }
      stackDepth += 1;
      continue;
    }
    return null;
  }

  if (stackDepth !== 1) return null;
  code.push(0x0b); // end

  // local decls: 3 f64 locals
  const locals = [
    ...encodeU32(1), // decl count
    ...encodeU32(3),
    0x7c,            // f64
  ];
  const body = [...locals, ...code];
  return [...encodeU32(body.length), ...body];
}

function toBase64(bytes) {
  const arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (typeof btoa === 'function') {
    let bin = '';
    for (let i = 0; i < arr.length; i++) {
      bin += String.fromCharCode(arr[i]);
    }
    return btoa(bin);
  }
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(arr).toString('base64');
  }
  throw new Error('No base64 encoder available');
}

function buildReactiveWasm(programByExprId) {
  const exprIds = Object.keys(programByExprId);
  if (!exprIds.length) return null;

  const { pathToId, idToPath } = gatherPathIds(programByExprId);
  const functionBodies = [];
  const exprExports = Object.create(null);

  for (let i = 0; i < exprIds.length; i++) {
    const exprId = exprIds[i];
    const program = programByExprId[exprId];
    const body = compileProgramToWasmBody(program, pathToId);
    if (!body) continue;
    functionBodies.push({ exprId, body });
    exprExports[exprId] = `e${exprId}`;
  }

  if (!functionBodies.length) return null;

  const bytes = [];
  bytes.push(0x00, 0x61, 0x73, 0x6d); // \0asm
  bytes.push(0x01, 0x00, 0x00, 0x00); // version

  const typePayload = [
    ...encodeU32(2),
    0x60, ...encodeU32(1), 0x7f, ...encodeU32(1), 0x7c, // (i32) -> f64
    0x60, ...encodeU32(0), ...encodeU32(1), 0x7c,       // () -> f64
  ];
  bytes.push(...makeSection(1, typePayload));

  const importPayload = [
    ...encodeU32(1),
    ...encodeName('env'),
    ...encodeName('host_get'),
    0x00, ...encodeU32(0), // kind func, type 0
  ];
  bytes.push(...makeSection(2, importPayload));

  const funcPayload = [...encodeU32(functionBodies.length)];
  for (let i = 0; i < functionBodies.length; i++) {
    funcPayload.push(...encodeU32(1)); // each function uses type idx 1
  }
  bytes.push(...makeSection(3, funcPayload));

  const exportPayload = [...encodeU32(functionBodies.length)];
  for (let i = 0; i < functionBodies.length; i++) {
    const { exprId } = functionBodies[i];
    const exportName = exprExports[exprId];
    exportPayload.push(...encodeName(exportName));
    exportPayload.push(0x00); // func
    exportPayload.push(...encodeU32(1 + i)); // import func count offset
  }
  bytes.push(...makeSection(7, exportPayload));

  const codePayload = [...encodeU32(functionBodies.length)];
  for (let i = 0; i < functionBodies.length; i++) {
    codePayload.push(...functionBodies[i].body);
  }
  bytes.push(...makeSection(10, codePayload));

  return {
    wasmBase64: toBase64(new Uint8Array(bytes)),
    pathById: idToPath,
    exprExports,
  };
}

export function buildReactiveWasmPayloadFromDeps(deps) {
  if (!deps || !deps.length) return null;
  const programByExprId = Object.create(null);
  for (let i = 0; i < deps.length; i++) {
    const dep = deps[i];
    const exprBind = parseExprBind(dep);
    if (exprBind && isNumericCompatible(exprBind.program)) {
      programByExprId[exprBind.exprId] = exprBind.program;
      continue;
    }
    const exprAttr = parseExprAttr(dep);
    if (exprAttr && isNumericCompatible(exprAttr.program)) {
      programByExprId[exprAttr.exprId] = exprAttr.program;
    }
  }

  const compiled = buildReactiveWasm(programByExprId);
  if (!compiled) return null;
  return {
    version: 1,
    ...compiled,
  };
}

export function buildReactiveWasmPayload(bytecode) {
  const deps = extractDependencyMarkersFromBytecode(bytecode);
  return buildReactiveWasmPayloadFromDeps(deps);
}
