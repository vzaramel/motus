/**
 * Motus WASM Host Interface
 *
 * Provides the host functions that the WASM runtime imports.
 * This runs in the JavaScript environment (browser, Node.js, or Cloudflare Worker).
 */
import { normalizeComponentRef, normalizeDataRequirement, normalizeU32 } from './runtime-contract.js';

/**
 * Create a host environment for the WASM runtime
 * @param {Object} options - Configuration options
 * @param {WritableStreamDefaultWriter} options.writer - Stream writer for HTML output
 * @param {TextEncoder} options.encoder - Text encoder
 * @param {Function} options.fetchData - Function to fetch data from origin
 * @param {Function} options.onDependency - Callback when a dependency is tracked
 */
export function createHost(options) {
  const {
    writer,
    encoder,
    fetchData,
    loadComponent,
    onDependency,
    onDebugStep,
    pauseOnDebugStep = false,
    defaultSourcePath = '',
  } = options;

  // WASM memory reference (set after instantiation)
  let memory = null;

  // Output buffer for batching writes
  let outputBuffer = '';
  const FLUSH_THRESHOLD = 1024;

  // Pending data requests
  const pendingRequests = new Map();
  let nextRequestId = 1;

  // Current dependency being tracked
  let currentDep = null;
  let debugStepPauseEnabled = pauseOnDebugStep === true;
  const debugLineByChunk = new Map();

  function pauseAtSourceStep(event) {
    if (!debugStepPauseEnabled) return;
    const line = Number(event?.line ?? 0) | 0;
    const sourcePathRaw = String(event?.sourcePath || '');
    const sourcePath = sourcePathRaw.replace(/[\r\n]/g, '').trim();
    if (!sourcePath || line <= 0 || typeof eval !== 'function') {
      debugger;
      return;
    }
    try {
      const safeLine = Math.min(Math.max(line, 1), 50000);
      const script = `${'\n'.repeat(safeLine - 1)}debugger;\n//# sourceURL=${sourcePath}`;
      (0, eval)(script);
    } catch {
      debugger;
    }
  }

  /**
   * Read a string from WASM memory
   */
  function readString(ptr, len) {
    if (!memory) return '';
    const max = memory.buffer.byteLength - ptr;
    if (max <= 0) return '';
    const size = Math.min(len, max);
    const bytes = new Uint8Array(memory.buffer, ptr, size);
    return new TextDecoder().decode(bytes);
  }

  /**
   * Flush output buffer to stream
   */
  async function flush() {
    if (outputBuffer.length > 0) {
      await writer.write(encoder.encode(outputBuffer));
      outputBuffer = '';
    }
  }

  /**
   * Host functions imported by WASM
   */
  const imports = {
    env: {
      /**
       * Output HTML content
       */
      host_output(ptr, len) {
        const text = readString(ptr, len);
        outputBuffer += text;
        if (outputBuffer.length >= FLUSH_THRESHOLD) {
          // Schedule flush but don't await (async in sync context)
          flush().catch(console.error);
        }
      },

      /**
       * Report an error
       */
      host_error(ptr, len) {
        const msg = readString(ptr, len);
        console.error('[mot-wasm] Error:', msg);
      },

      /**
       * Log a debug message
       */
      host_log(ptr, len) {
        const msg = readString(ptr, len);
        console.log('[mot-wasm]', msg);
      },

      /**
       * Render complete - flush and signal done
       */
      host_render_complete() {
        flush().then(() => {
          console.log('[mot-wasm] Render complete');
        }).catch(console.error);
      },

      /**
       * Start tracking a dependency
       */
      host_dep_start(ptr, len) {
        const path = readString(ptr, len);
        currentDep = path;
        if (onDependency) {
          onDependency('start', path);
        }
      },

      /**
       * End dependency tracking
       */
      host_dep_end() {
        if (currentDep && onDependency) {
          onDependency('end', currentDep);
        }
        currentDep = null;
      },

      host_component_start(_funcIdx) {
        // Reserved for debug attribution hooks in wasm VM.
      },

      host_component_end(_funcIdx) {
        // Reserved for debug attribution hooks in wasm VM.
      },

      host_slot_default_start(_funcIdx) {
        // Reserved for slot attribution hooks in wasm VM.
      },

      host_slot_default_end(_funcIdx) {
        // Reserved for slot attribution hooks in wasm VM.
      },

      host_debug_step(chunkKind, chunkIndex, pc, opcode, line, column, sourcePathPtr, sourcePathLen) {
        const rawSourcePath = readString(sourcePathPtr, sourcePathLen).replace(/\0.*$/, '').trim();
        const normalizedChunkKind = normalizeU32(chunkKind);
        const normalizedChunkIndex = normalizeU32(chunkIndex);
        let normalizedLine = normalizeU32(line);
        const debugKey = `${normalizedChunkKind}:${normalizedChunkIndex}`;
        if (normalizedLine > 0) {
          debugLineByChunk.set(debugKey, normalizedLine);
        } else if (debugLineByChunk.has(debugKey)) {
          normalizedLine = debugLineByChunk.get(debugKey);
        }
        const event = {
          chunkKind: normalizedChunkKind,
          chunkIndex: normalizedChunkIndex,
          pc: normalizeU32(pc),
          opcode: normalizeU32(opcode),
          line: normalizedLine,
          column: normalizeU32(column),
          sourcePath: rawSourcePath || String(defaultSourcePath || ''),
        };
        if (onDebugStep) {
          try {
            onDebugStep(event);
          } catch (err) {
            console.error('[mot-wasm] debug step callback failed:', err);
          }
        }
        pauseAtSourceStep(event);
      },

      /**
       * Request data from origin
       */
      host_fetch_data(reqId, queryRef, signature, namePtr, paramsPtr, isSingle) {
        const reqIdU32 = normalizeU32(reqId);
        const normalizedReq = normalizeDataRequirement({ queryRef, signature, isSingle });
        const name = readString(namePtr, 256).replace(/\0.*$/, '');
        const paramsJson = readString(paramsPtr, 2048).replace(/\0.*$/, '');
        let params = {};
        try {
          params = paramsJson ? JSON.parse(paramsJson) : {};
        } catch {
          params = {};
        }

        if (fetchData) {
          Promise.resolve(
            fetchData({
              reqId: reqIdU32,
              queryRef: normalizedReq.queryRef,
              signature: normalizedReq.signature,
              name,
              isSingle: normalizedReq.isSingle,
            }, params),
          ).catch((err) => {
            console.error('[mot-wasm] fetchData failed:', err);
          });
        } else {
          console.log('[mot-wasm] Data fetch requested:', {
            reqId: reqIdU32,
            queryRef: normalizedReq.queryRef,
            signature: normalizedReq.signature,
            name,
            isSingle: normalizedReq.isSingle,
          });
        }
      },

      host_load_component(reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
        const opcode = 'BC_COMPONENT_LOAD';
        const reqIdU32 = normalizeU32(reqId);
        const name = readString(namePtr, 256).replace(/\0.*$/, '');
        const path = readString(pathPtr, 512).replace(/\0.*$/, '');
        const propsJson = readString(propsPtr, 4096).replace(/\0.*$/, '');
        const childrenJson = readString(childrenPtr, 4096).replace(/\0.*$/, '');
        let normalizedRef = null;
        let props = null;
        let children = null;
        try {
          normalizedRef = normalizeComponentRef({ name, path });
        } catch (err) {
          console.error('[mot-wasm] invalid component ref:', err?.message || String(err));
          return;
        }
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
        if (loadComponent) {
          Promise.resolve(
            loadComponent({
              reqId: reqIdU32,
              name: normalizedRef.name,
              path: normalizedRef.path,
              props,
              children,
              componentOpcode: opcode,
            }),
          ).catch((err) => {
            console.error('[mot-wasm] loadComponent failed:', err);
          });
          return;
        }
        console.log('[mot-wasm] Component load requested:', {
          reqId: reqIdU32,
          name: normalizedRef.name,
          path: normalizedRef.path,
          props,
          children,
          componentOpcode: opcode,
        });
      },

      host_load_component_linked(reqId, namePtr, pathPtr, propsPtr, childrenPtr) {
        const opcode = 'BC_COMPONENT_LINKED';
        const reqIdU32 = normalizeU32(reqId);
        const name = readString(namePtr, 256).replace(/\0.*$/, '');
        const path = readString(pathPtr, 512).replace(/\0.*$/, '');
        const propsJson = readString(propsPtr, 4096).replace(/\0.*$/, '');
        const childrenJson = readString(childrenPtr, 4096).replace(/\0.*$/, '');
        let normalizedRef = null;
        let props = null;
        let children = null;
        try {
          normalizedRef = normalizeComponentRef({ name, path });
        } catch (err) {
          console.error('[mot-wasm] invalid component ref:', err?.message || String(err));
          return;
        }
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
        if (loadComponent) {
          Promise.resolve(
            loadComponent({
              reqId: reqIdU32,
              name: normalizedRef.name,
              path: normalizedRef.path,
              props,
              children,
              componentOpcode: opcode,
            }),
          ).catch((err) => {
            console.error('[mot-wasm] loadComponent failed:', err);
          });
          return;
        }
        console.log('[mot-wasm] Component load requested:', {
          reqId: reqIdU32,
          name: normalizedRef.name,
          path: normalizedRef.path,
          props,
          children,
          componentOpcode: opcode,
        });
      },

    },
  };

  return {
    imports,

    /**
     * Set the WASM memory reference
     */
    setMemory(mem) {
      memory = mem;
    },

    /**
     * Flush any remaining output
     */
    async flush() {
      await flush();
    },

    /**
     * Provide data for a pending request
     */
    provideData(reqId, data) {
      const callback = pendingRequests.get(reqId);
      if (callback) {
        callback(data);
        pendingRequests.delete(reqId);
      }
    },

    /**
     * Pause JS execution on each wasm VM debug step.
     */
    setPauseOnDebugStep(enabled) {
      debugStepPauseEnabled = enabled === true;
    },
  };
}

/**
 * Load and instantiate the WASM runtime
 * @param {ArrayBuffer|Uint8Array} wasmBytes - The WASM module bytes
 * @param {Object} host - Host interface created by createHost()
 * @returns {Object} The WASM instance exports
 */
export async function instantiateRuntime(wasmBytes, host) {
  const { instance } = await WebAssembly.instantiate(wasmBytes, host.imports);
  const textEncoder = new TextEncoder();

  // Set memory reference in host
  if (instance.exports.memory) {
    host.setMemory(instance.exports.memory);
  }

  return {
    /**
     * Initialize the runtime with bytecode
     * @param {Uint8Array} bytecode - The compiled bytecode
     * @returns {number} 0 on success, -1 on error
     */
    init(bytecode) {
      if (!instance.exports.mot_alloc || !instance.exports.mot_init) {
        console.error('[mot-wasm] Missing required exports: mot_alloc/mot_init');
        return -1;
      }

      if (instance.exports.mot_reset_alloc) {
        instance.exports.mot_reset_alloc();
      }

      const ptr = instance.exports.mot_alloc(bytecode.length);
      if (!ptr) {
        console.error('[mot-wasm] mot_alloc failed');
        return -1;
      }

      if (!instance.exports.memory) {
        console.error('[mot-wasm] WASM memory export missing');
        return -1;
      }

      const mem = new Uint8Array(instance.exports.memory.buffer);
      mem.set(bytecode, ptr);

      return instance.exports.mot_init(ptr, bytecode.length);
    },

    /**
     * Start rendering
     * @returns {number} 0 on success, -1 on error
     */
    render() {
      if (instance.exports.mot_render) {
        return instance.exports.mot_render();
      }
      return -1;
    },

    /**
     * Render a component function with JSON-serializable props/children
     * @param {number} functionIndex - Function chunk index (component body)
     * @param {any} props - Component props payload
     * @param {any} children - Component children payload
     * @returns {number} 0 on success, -2 on await, -1 on error
     */
    renderComponent(functionIndex = 0, props = null, children = null) {
      if (!instance.exports.mot_render_component || !instance.exports.mot_alloc || !instance.exports.memory) {
        return -1;
      }
      const propsBytes = textEncoder.encode(JSON.stringify(props));
      const childrenBytes = textEncoder.encode(JSON.stringify(children));

      const propsPtr = instance.exports.mot_alloc(propsBytes.length + 1);
      const childrenPtr = instance.exports.mot_alloc(childrenBytes.length + 1);
      if (!propsPtr || !childrenPtr) {
        return -1;
      }

      const mem = new Uint8Array(instance.exports.memory.buffer);
      mem.set(propsBytes, propsPtr);
      mem[propsPtr + propsBytes.length] = 0;
      mem.set(childrenBytes, childrenPtr);
      mem[childrenPtr + childrenBytes.length] = 0;

      return instance.exports.mot_render_component(
        functionIndex >>> 0,
        propsPtr,
        propsBytes.length,
        childrenPtr,
        childrenBytes.length,
      );
    },

    /**
     * Get current state
     * @returns {number} 0=idle, 1=running, 2=done, 3=error
     */
    state() {
      if (instance.exports.mot_state) {
        return instance.exports.mot_state();
      }
      return 3; // error
    },

    /**
     * Enable/disable wasm VM debug-step callbacks.
     */
    setDebugStepMode(enabled) {
      if (!instance.exports.mot_set_debug_step_mode) return;
      instance.exports.mot_set_debug_step_mode(enabled ? 1 : 0);
    },

    /**
     * Enable/disable DevTools pause on each debug step callback.
     */
    setPauseOnDebugStep(enabled) {
      if (host && typeof host.setPauseOnDebugStep === 'function') {
        host.setPauseOnDebugStep(enabled === true);
      }
    },

    /**
     * Free resources
     */
    free() {
      if (instance.exports.mot_free) {
        instance.exports.mot_free();
      }
    },

    // Raw exports for advanced usage
    exports: instance.exports,
  };
}
