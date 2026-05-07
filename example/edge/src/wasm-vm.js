/**
 * WASM VM adapter for Cloudflare Workers.
 *
 * This uses the shared C VM compiled to wasm and supports async resume for:
 * - BC_FETCH_DATA (origin RPC)
 * - BC_COMPONENT_LOAD (dynamic component render via host/origin)
 */
import { normalizeComponentRef, normalizeDataRequirement, normalizeU32 } from './runtime-contract.js';

const BYTECODE_MAGIC = 0x00544F4D;
const BYTECODE_VERSION_MAJOR = 1;
const BYTECODE_VERSION_MINOR = 2;
const MOT_OK = 0;
const MOT_AWAIT = -2;

function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

function readU16(view, offset) {
  return view.getUint16(offset, true);
}

function readU32(view, offset) {
  return view.getUint32(offset, true);
}

function operandSize(op) {
  switch (op) {
    case 1:   // BC_CONST
    case 4:   // BC_LOAD
    case 5:   // BC_STORE
    case 6:   // BC_LOAD_GLOBAL
    case 7:   // BC_LOAD_FIELD
    case 9:   // BC_STORE_FIELD
    case 14:  // BC_INT
    case 30:  // BC_JUMP
    case 31:  // BC_JUMP_IF_FALSE
    case 32:  // BC_JUMP_IF_TRUE
    case 34:  // BC_ITER_NEXT
    case 36:  // BC_EMIT_LITERAL
    case 39:  // BC_EMIT_ATTR_START
    case 41:  // BC_EMIT_TAG_OPEN
    case 43:  // BC_EMIT_TAG_CLOSE
    case 45:  // BC_FETCH_DATA
    case 51:  // BC_COMPONENT_START
    case 54:  // BC_SLOT_START
    case 57:  // BC_DEP_START
    case 59:  // BC_ARRAY_NEW
    case 60:  // BC_OBJECT_NEW
    case 61:  // BC_OBJECT_SET
      return 2;
    case 47:  // BC_CALL
    case 48:  // BC_CALL_BUILTIN
    case 53:  // BC_COMPONENT_LOAD
    case 64:  // BC_COMPONENT_LINKED
      return 3;
    case 49:  // BC_CALL_PIPE
      return 2;
    case 62:  // BC_CONCAT
      return 1;
    default:
      return 0;
  }
}

function scanChunkForUnsupported(bytes, start, len, reasons) {
  let ip = start;
  const end = start + len;

  while (ip < end) {
    const op = bytes[ip++];

    const skip = operandSize(op);
    ip += skip;
    if (ip > end) {
      reasons.push('INVALID_OPCODE_STREAM');
      return;
    }
  }
}

export function detectWasmIncompatibilities(bytecode) {
  const reasons = [];
  try {
    const bytes = bytecode instanceof Uint8Array ? bytecode : new Uint8Array(bytecode);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let ip = 0;

    if (bytes.byteLength < 4 + 2 + 2 + 2 + 7 * 4) {
      return { unsupported: true, reasons: ['BYTECODE_TOO_SHORT'] };
    }

    const magic = readU32(view, ip); ip += 4;
    const major = readU16(view, ip); ip += 2;
    const minor = readU16(view, ip); ip += 2;
    ip += 2; // flags

    if (magic !== BYTECODE_MAGIC) {
      return { unsupported: true, reasons: ['INVALID_MAGIC'] };
    }
    if (major !== BYTECODE_VERSION_MAJOR || minor !== BYTECODE_VERSION_MINOR) {
      return { unsupported: true, reasons: ['UNSUPPORTED_VERSION'] };
    }

    const constCount = readU32(view, ip); ip += 4;
    const stringCount = readU32(view, ip); ip += 4;
    const dataReqCount = readU32(view, ip); ip += 4;
    const depCount = readU32(view, ip); ip += 4;
    const builtinCount = readU32(view, ip); ip += 4;
    const compRefCount = readU32(view, ip); ip += 4;
    const funcCount = readU32(view, ip); ip += 4;

    // constants
    for (let i = 0; i < constCount; i++) {
      const type = bytes[ip++];
      if (type === 0 || type === 1) {
        ip += 1;
      } else if (type === 2 || type === 3) {
        ip += 8;
      } else if (type === 4) {
        const len = readU32(view, ip); ip += 4 + len;
      } else {
        return { unsupported: true, reasons: ['INVALID_CONST_TYPE'] };
      }
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_CONSTANTS'] };
    }

    // strings
    for (let i = 0; i < stringCount; i++) {
      const len = readU32(view, ip); ip += 4 + len;
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_STRINGS'] };
    }

    // data reqs
    for (let i = 0; i < dataReqCount; i++) {
      const nameLen = readU32(view, ip); ip += 4 + nameLen;
      ip += 2; // flags
      ip += 4; // query_ref
      ip += 4; // signature
      const paramCount = readU16(view, ip); ip += 2;
      for (let p = 0; p < paramCount; p++) {
        const pNameLen = readU32(view, ip); ip += 4 + pNameLen;
        ip += 2; // slot
      }
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_DATA_REQS'] };
    }

    // deps
    for (let i = 0; i < depCount; i++) {
      const pathLen = readU32(view, ip); ip += 4 + pathLen;
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_DEPS'] };
    }

    // builtins
    for (let i = 0; i < builtinCount; i++) {
      const bLen = readU32(view, ip); ip += 4 + bLen;
      ip += 2; // min/max args
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_BUILTINS'] };
    }

    // component refs
    for (let i = 0; i < compRefCount; i++) {
      const nLen = readU32(view, ip); ip += 4 + nLen;
      const pLen = readU32(view, ip); ip += 4 + pLen;
      if (ip > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_COMPONENT_REFS'] };
    }

    // main chunk
    const mainLen = readU32(view, ip); ip += 4;
    if (ip + mainLen > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_MAIN_CHUNK'] };
    scanChunkForUnsupported(bytes, ip, mainLen, reasons);
    ip += mainLen;

    // function chunks
    for (let i = 0; i < funcCount; i++) {
      const fnLen = readU32(view, ip); ip += 4;
      if (ip + fnLen > bytes.byteLength) return { unsupported: true, reasons: ['TRUNCATED_FUNCTION_CHUNK'] };
      scanChunkForUnsupported(bytes, ip, fnLen, reasons);
      ip += fnLen;
    }

    return { unsupported: reasons.length > 0, reasons };
  } catch (err) {
    return { unsupported: true, reasons: [`PARSE_ERROR:${err.message}`] };
  }
}

function readBytes(memory, ptr, len) {
  const mem = new Uint8Array(memory.buffer);
  if (ptr < 0 || ptr >= mem.length) return new Uint8Array(0);
  const end = Math.min(ptr + len, mem.length);
  return mem.subarray(ptr, end);
}

function readCString(memory, ptr, maxLen = 4096) {
  const mem = new Uint8Array(memory.buffer);
  if (ptr < 0 || ptr >= mem.length) return '';
  let end = ptr;
  const limit = Math.min(mem.length, ptr + maxLen);
  while (end < limit && mem[end] !== 0) end++;
  return DECODER.decode(mem.subarray(ptr, end));
}

const DECODER = new TextDecoder();
const ENCODER = new TextEncoder();

function deriveComponentPathFromSource(sourcePath) {
  const src = String(sourcePath || '').trim();
  if (!src) return '';
  if (src.startsWith('components/') || src.startsWith('pages/')) {
    if (src.endsWith('.mot')) return src.slice(0, -4);
    return src;
  }
  return '';
}

function writeString(instance, memory, text) {
  const encoded = ENCODER.encode(text ?? '');
  const ptr = instance.exports.mot_alloc(encoded.length + 1);
  if (!ptr) {
    throw new Error('mot_alloc failed for resume payload');
  }
  const mem = new Uint8Array(memory.buffer);
  mem.set(encoded, ptr);
  mem[ptr + encoded.length] = 0;
  return { ptr, len: encoded.length };
}

/**
 * Convert a component path like "components/ProductCard" to a kebab-case
 * custom element tag name: "mot-product-card".
 */
export function componentPathToTagName(path) {
  // Extract last segment: "components/ProductCard" → "ProductCard"
  const name = path.includes('/') ? path.split('/').pop() : path;
  // PascalCase → kebab-case + sanitize any invalid chars (#, etc) to '-'
  const kebab = name
    .replace(/([a-z0-9])([A-Z])/g, '$1-$2')
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1-$2')
    .toLowerCase()
    .replace(/[^a-z0-9-]/g, '-')
    .replace(/^-+|-+$/g, '')
    .replace(/-+/g, '-');
  return `mot-${kebab || 'anon'}`;
}

export async function renderBytecodeWithWasm(options) {
  const {
    wasmModule,
    bytecode,
    output,
    fetchData,
    onDependency,
    loadComponent,
    onComponentRender,
    component,
    componentDebugMetaByFunction = null,
    enableComponentDebugWrap = false,
    enableVmSourceStepping = false,
    onDebugStep = null,
    fetchScope = null,
    onVmAwait = null,
    onVmResume = null,
    enableDSD = false,
  } = options;
  const isComponentRender = component != null;

  if (!wasmModule) {
    return { success: false, error: 'WASM module not available' };
  }

  const state = {
    instance: null,
    memory: null,
    html: '',
    error: '',
    depPath: null,
    pendingResume: null,
    pendingMeta: null,
    componentFrames: [],
  };

  const resumeWithPayload = async (reqId, payloadText) => {
    const { ptr, len } = writeString(state.instance, state.memory, payloadText);
    return state.instance.exports.mot_resume(reqId, ptr, len);
  };
  const emitComponentRender = (event) => {
    if (typeof onComponentRender !== 'function') return;
    try {
      onComponentRender(event);
    } catch (err) {
      console.warn(`[edge][wasm] component render trace callback failed: ${err?.message || String(err)}`);
    }
  };
  const resolveStaticComponentMeta = (funcIdx) => {
    const normalizedFuncIdx = normalizeU32(funcIdx);
    const meta = componentDebugMetaByFunction
      ? componentDebugMetaByFunction[String(normalizedFuncIdx)]
      : null;
    const sourcePath = meta?.sourcePath || '';
    const componentPath = meta?.componentPath || deriveComponentPathFromSource(sourcePath);
    return {
      funcIdx: normalizedFuncIdx,
      path: componentPath || `component#${normalizedFuncIdx}`,
      renderEngine: meta?.renderEngine || 'wasm-vm',
      loadSource: meta?.loadSource || 'static',
      artifactSource: meta?.artifactSource || 'inline',
      callKind: meta?.callKind || 'static_origin',
      componentOpcode: meta?.componentOpcode || 'BC_COMPONENT_START',
      sizeBytes: Number(meta?.sizeBytes ?? 0),
    };
  };

  const imports = {
    env: {
      host_output(ptr, len) {
        const bytes = readBytes(state.memory, ptr, len);
        state.html += DECODER.decode(bytes);
      },
      host_error(ptr, len) {
        const bytes = readBytes(state.memory, ptr, len);
        state.error = DECODER.decode(bytes);
      },
      host_log(ptr, len) {
        const bytes = readBytes(state.memory, ptr, len);
        const msg = DECODER.decode(bytes);
        console.log('[edge][wasm]', msg);
      },
      host_render_complete() {
        // no-op
      },
      host_dep_start(ptr, len) {
        const bytes = readBytes(state.memory, ptr, len);
        state.depPath = DECODER.decode(bytes);
        if (onDependency && state.depPath) {
          onDependency('start', state.depPath);
        }
      },
      host_dep_end() {
        if (onDependency && state.depPath) {
          onDependency('end', state.depPath);
        }
        state.depPath = null;
      },
      host_component_start(funcIdx) {
        const frameMeta = resolveStaticComponentMeta(funcIdx);
        state.componentFrames.push({
          funcIdx: frameMeta.funcIdx,
          startedAt: nowMs(),
          meta: frameMeta,
        });
        // DSD wrap: open custom element + declarative shadow root.
        // Skip the page-level root frame; everything else gets wrapped.
        if (enableDSD && frameMeta.path && !frameMeta.path.startsWith('pages/')) {
          const tag = componentPathToTagName(frameMeta.path);
          state.html += `<${tag} data-mot-path="${frameMeta.path}"><template shadowrootmode="open">`;
          state.dsdWrapped = state.dsdWrapped || new Set();
          state.dsdWrapped.add(frameMeta.funcIdx);
        }
        emitComponentRender({
          phase: 'start',
          status: 'ok',
          ...frameMeta,
        });
      },
      host_component_end(funcIdx) {
        const normalizedFuncIdx = normalizeU32(funcIdx);
        let frame = null;
        for (let i = state.componentFrames.length - 1; i >= 0; i--) {
          if (state.componentFrames[i].funcIdx === normalizedFuncIdx) {
            frame = state.componentFrames[i];
            state.componentFrames.splice(i, 1);
            break;
          }
        }
        const startedAt = frame?.startedAt ?? nowMs();
        const frameMeta = frame?.meta || resolveStaticComponentMeta(normalizedFuncIdx);
        // DSD wrap: close shadow template + custom element (only if start wrapped)
        if (enableDSD && state.dsdWrapped && state.dsdWrapped.has(normalizedFuncIdx)) {
          const tag = componentPathToTagName(frameMeta.path || `component-${normalizedFuncIdx}`);
          state.html += `</template></${tag}>`;
          state.dsdWrapped.delete(normalizedFuncIdx);
        }
        emitComponentRender({
          phase: 'end',
          path: frameMeta.path,
          status: 'ok',
          durationMs: nowMs() - startedAt,
          renderEngine: frameMeta.renderEngine,
          loadSource: frameMeta.loadSource,
          artifactSource: frameMeta.artifactSource,
          callKind: frameMeta.callKind,
          componentOpcode: frameMeta.componentOpcode,
          sizeBytes: frameMeta.sizeBytes,
        });
      },
      host_slot_default_start(funcIdx) {
        void funcIdx;
        // Slot markers are emitted by the wasm runtime alongside output.
      },
      host_slot_default_end(funcIdx) {
        void funcIdx;
        // Slot markers are emitted by the wasm runtime alongside output.
      },
      host_debug_step(chunkKind, chunkIndex, pc, opcode, line, column, sourcePathPtr, sourcePathLen) {
        if (typeof onDebugStep !== 'function') return;
        try {
          const sourceBytes = readBytes(state.memory, sourcePathPtr, sourcePathLen);
          onDebugStep({
            chunkKind: normalizeU32(chunkKind),
            chunkIndex: normalizeU32(chunkIndex),
            pc: normalizeU32(pc),
            opcode: normalizeU32(opcode),
            line: normalizeU32(line),
            column: normalizeU32(column),
            sourcePath: DECODER.decode(sourceBytes),
          });
        } catch (err) {
          console.warn(`[edge][wasm] debug step callback failed: ${err?.message || String(err)}`);
        }
      },
      host_fetch_data(reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
        const reqIdU32 = normalizeU32(reqId);
        const normalizedReq = normalizeDataRequirement({
          queryRef,
          signature,
          isSingle,
        });
        const name = readCString(state.memory, namePtr, 256);
        const paramsJson = readCString(state.memory, paramsPtr, 4096);

        if (state.pendingResume) {
          state.pendingResume = Promise.reject(new Error('Concurrent async wasm fetch is not supported'));
          return;
        }

        let params = {};
        try {
          params = paramsJson ? JSON.parse(paramsJson) : {};
        } catch {
          params = {};
        }

        const activeFrame = state.componentFrames.length > 0
          ? state.componentFrames[state.componentFrames.length - 1]
          : null;
        const activeMeta = activeFrame?.meta || null;
        const fallbackScope = fetchScope && typeof fetchScope === 'object' ? fetchScope : null;
        const fallbackCallKind = fallbackScope?.componentCallKind
          ? String(fallbackScope.componentCallKind)
          : 'page';
        const fallbackIsDelayed =
          fallbackCallKind === 'dynamic_vm' ||
          fallbackCallKind === 'linked_vm' ||
          fallbackCallKind === 'linked_edge';
        const componentPath =
          activeMeta?.path ||
          (fallbackScope?.componentPath ? String(fallbackScope.componentPath) : '');
        const componentCallKind = fallbackIsDelayed
          ? fallbackCallKind
          : (activeMeta?.callKind || fallbackCallKind);
        state.pendingMeta = {
          kind: 'data_fetch',
          reqId: reqIdU32,
          queryRef: normalizedReq.queryRef,
          signature: normalizedReq.signature,
          name,
          componentPath,
          componentCallKind,
        };

        state.pendingResume = Promise.resolve(fetchData?.({
            queryRef: normalizedReq.queryRef,
            signature: normalizedReq.signature,
            name,
            isSingle: normalizedReq.isSingle,
          }, params, {
            componentPath,
            componentCallKind,
          }))
          .then((data) => ({
            reqId: reqIdU32,
            payloadText: JSON.stringify(data ?? null),
          }))
          .catch((err) => {
            throw new Error(`fetchData callback failed: ${err?.message || String(err)}`);
          });
      },
      host_load_component(reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
        const componentOpcode = 'BC_COMPONENT_LOAD';
        const reqIdU32 = normalizeU32(reqId);
        const name = readCString(state.memory, namePtr, 256);
        const path = readCString(state.memory, pathPtr, 512);
        const propsJson = readCString(state.memory, propsPtr, 4096);
        const childrenJson = readCString(state.memory, childrenPtr, 4096);

        if (state.pendingResume) {
          state.pendingResume = Promise.reject(new Error('Concurrent async wasm component load is not supported'));
          return;
        }

        let props = null;
        let children = null;
        try {
          props = propsJson ? JSON.parse(propsJson) : null;
        } catch {
          props = null;
        }
        try {
          children = childrenJson ? JSON.parse(childrenJson) : null;
        } catch {
          children = null;
        }

        let normalizedRef;
        try {
          normalizedRef = normalizeComponentRef({ name, path });
        } catch (err) {
          state.pendingResume = Promise.reject(new Error(`invalid component ref: ${err?.message || String(err)}`));
          return;
        }
        const activeFrame = state.componentFrames.length > 0
          ? state.componentFrames[state.componentFrames.length - 1]
          : null;
        const activeMeta = activeFrame?.meta || null;
        state.pendingMeta = {
          kind: 'component_load',
          reqId: reqIdU32,
          name: normalizedRef.name,
          componentPath: normalizedRef.path,
          componentOpcode,
          componentCallKind: activeMeta?.callKind || 'page',
        };

        state.pendingResume = Promise.resolve(loadComponent?.({
          name: normalizedRef.name,
          path: normalizedRef.path,
          props,
          children,
          componentOpcode,
        }))
          .then((html) => ({
            reqId: reqIdU32,
            payloadText: String(html ?? ''),
          }))
          .catch((err) => {
            throw new Error(`component load failed: ${err?.message || String(err)}`);
          });
      },
      host_load_component_linked(reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
        const componentOpcode = 'BC_COMPONENT_LINKED';
        const reqIdU32 = normalizeU32(reqId);
        const name = readCString(state.memory, namePtr, 256);
        const path = readCString(state.memory, pathPtr, 512);
        const propsJson = readCString(state.memory, propsPtr, 4096);
        const childrenJson = readCString(state.memory, childrenPtr, 4096);

        if (state.pendingResume) {
          state.pendingResume = Promise.reject(new Error('Concurrent async wasm component load is not supported'));
          return;
        }

        let props = null;
        let children = null;
        try {
          props = propsJson ? JSON.parse(propsJson) : null;
        } catch {
          props = null;
        }
        try {
          children = childrenJson ? JSON.parse(childrenJson) : null;
        } catch {
          children = null;
        }

        let normalizedRef;
        try {
          normalizedRef = normalizeComponentRef({ name, path });
        } catch (err) {
          state.pendingResume = Promise.reject(new Error(`invalid component ref: ${err?.message || String(err)}`));
          return;
        }
        const activeFrame = state.componentFrames.length > 0
          ? state.componentFrames[state.componentFrames.length - 1]
          : null;
        const activeMeta = activeFrame?.meta || null;
        state.pendingMeta = {
          kind: 'component_load',
          reqId: reqIdU32,
          name: normalizedRef.name,
          componentPath: normalizedRef.path,
          componentOpcode,
          componentCallKind: activeMeta?.callKind || 'page',
        };

        state.pendingResume = Promise.resolve(loadComponent?.({
          name: normalizedRef.name,
          path: normalizedRef.path,
          props,
          children,
          componentOpcode,
        }))
          .then((html) => ({
            reqId: reqIdU32,
            payloadText: String(html ?? ''),
          }))
          .catch((err) => {
            throw new Error(`component load failed: ${err?.message || String(err)}`);
          });
      },
    },
  };

  try {
    const instantiated = await WebAssembly.instantiate(wasmModule, imports);
    const instance =
      instantiated instanceof WebAssembly.Instance
        ? instantiated
        : instantiated.instance;

    if (!instance) {
      return { success: false, error: 'Failed to instantiate wasm module' };
    }

    state.instance = instance;
    state.memory = instance.exports.memory;

    if (!state.memory || !instance.exports.mot_alloc || !instance.exports.mot_init) {
      return { success: false, error: 'Missing required wasm exports' };
    }
    if (!isComponentRender && !instance.exports.mot_render) {
      return { success: false, error: 'Missing required wasm export: mot_render' };
    }
    if (isComponentRender && !instance.exports.mot_render_component) {
      return { success: false, error: 'Missing required wasm export: mot_render_component' };
    }

    if (instance.exports.mot_reset_alloc) {
      instance.exports.mot_reset_alloc();
    }

    const ptr = instance.exports.mot_alloc(bytecode.length);
    if (!ptr) {
      return { success: false, error: 'mot_alloc failed' };
    }

    new Uint8Array(state.memory.buffer).set(bytecode, ptr);

    const initRes = instance.exports.mot_init(ptr, bytecode.length);
    if (initRes !== 0) {
      return { success: false, error: state.error || `mot_init failed (${initRes})` };
    }
    if (instance.exports.mot_set_debug_component_markers) {
      instance.exports.mot_set_debug_component_markers(enableComponentDebugWrap ? 1 : 0);
    }
    if (instance.exports.mot_set_debug_step_mode) {
      instance.exports.mot_set_debug_step_mode(enableVmSourceStepping ? 1 : 0);
    }

    let rc;
    if (isComponentRender) {
      const functionIndex = Number(component.functionIndex ?? 0);
      if (!Number.isInteger(functionIndex) || functionIndex < 0 || functionIndex > 0xFFFF) {
        return { success: false, error: `Invalid component function index: ${component.functionIndex}` };
      }
      const propsPayload = writeString(
        instance,
        state.memory,
        JSON.stringify(component.props ?? null),
      );
      const childrenPayload = writeString(
        instance,
        state.memory,
        JSON.stringify(component.children ?? null),
      );
      rc = instance.exports.mot_render_component(
        functionIndex,
        propsPayload.ptr,
        propsPayload.len,
        childrenPayload.ptr,
        childrenPayload.len,
      );
    } else {
      rc = instance.exports.mot_render();
    }
    while (rc === MOT_AWAIT) {
      const pending = state.pendingResume;
      const pendingMeta = state.pendingMeta || { kind: 'await' };
      if (!pending) {
        return { success: false, error: state.error || 'WASM requested resume but no pending operation exists' };
      }
      if (typeof onVmAwait === 'function') {
        try {
          onVmAwait({
            ...pendingMeta,
            atMs: nowMs(),
          });
        } catch (err) {
          console.warn(`[edge][wasm] vm await callback failed: ${err?.message || String(err)}`);
        }
      }
      state.pendingResume = null;
      state.pendingMeta = null;
      const pendingResult = await pending;
      if (typeof onVmResume === 'function') {
        try {
          onVmResume({
            ...pendingMeta,
            atMs: nowMs(),
            rc: null,
          });
        } catch (err) {
          console.warn(`[edge][wasm] vm resume callback failed: ${err?.message || String(err)}`);
        }
      }
      if (!pendingResult || typeof pendingResult !== 'object') {
        return { success: false, error: state.error || 'WASM resume payload missing' };
      }
      const resumeReqId = normalizeU32(pendingResult.reqId);
      const resumePayloadText = String(pendingResult.payloadText ?? '');
      rc = await resumeWithPayload(resumeReqId, resumePayloadText);
    }
    if (rc !== MOT_OK) {
      return { success: false, error: state.error || `mot_render/mot_resume failed (${rc})` };
    }

    if (output) {
      await output(state.html);
    }
    return { success: true, html: state.html };
  } catch (err) {
    return { success: false, error: err.message };
  } finally {
    if (state.instance && state.instance.exports && state.instance.exports.mot_free) {
      state.instance.exports.mot_free();
    }
  }
}

export async function renderComponentBytecodeWithWasm(options) {
  const {
    wasmModule,
    bytecode,
    fetchData,
    onDependency,
    loadComponent,
    onComponentRender,
    functionIndex = 0,
    props = null,
    children = null,
    componentDebugMetaByFunction = null,
    enableComponentDebugWrap = false,
    enableVmSourceStepping = false,
    onDebugStep = null,
    fetchScope = null,
    onVmAwait = null,
    onVmResume = null,
    enableDSD = false,
  } = options;

  let html = '';
  const result = await renderBytecodeWithWasm({
    wasmModule,
    bytecode,
    fetchData,
    onDependency,
    loadComponent,
    onComponentRender,
    componentDebugMetaByFunction,
    enableComponentDebugWrap,
    enableVmSourceStepping,
    onDebugStep,
    fetchScope,
    onVmAwait,
    onVmResume,
    enableDSD,
    component: { functionIndex, props, children },
    output: async (text) => {
      html += text;
    },
  });

  if (!result.success) return result;
  return { success: true, html };
}
