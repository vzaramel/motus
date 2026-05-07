/**
 * Motus WASM Runtime - JavaScript Host
 *
 * This module provides the host functions required by the WASM interpreter.
 * Use with the compiled mot-runtime.wasm module.
 *
 * Usage:
 *   const mot = await createMotus();
 *   const output = await mot.render(bytecode);
 */

class MotusHost {
  constructor() {
    this.output = [];
    this.dependencies = [];
    this.currentDeps = [];
    this.pendingFetches = new Map();
    this.fetchId = 0;
    this.onOutput = null;
    this.onFetch = null;
    this.onComplete = null;
    this.module = null;
  }

  /**
   * Initialize with the WASM module instance
   */
  setModule(module) {
    this.module = module;
  }

  /**
   * Host callback: output HTML content
   */
  host_output(dataPtr, len) {
    const data = this.module.UTF8ToString(dataPtr, len);
    this.output.push(data);
    if (this.onOutput) {
      this.onOutput(data, false);
    }
  }

  /**
   * Host callback: output escaped text
   */
  host_output_text(dataPtr, len) {
    const data = this.module.UTF8ToString(dataPtr, len);
    this.output.push(data);
    if (this.onOutput) {
      this.onOutput(data, true);
    }
  }

  /**
   * Host callback: request data fetch
   */
  host_fetch_data(reqId, namePtr, queryPtr) {
    const name = this.module.UTF8ToString(namePtr);
    const query = this.module.UTF8ToString(queryPtr);

    if (this.onFetch) {
      const promise = this.onFetch(name, query);
      this.pendingFetches.set(reqId, { name, query, promise });
    }
  }

  /**
   * Host callback: report error
   */
  host_error(msgPtr, len) {
    const msg = this.module.UTF8ToString(msgPtr, len);
    console.error('[Motus Error]', msg);
  }

  /**
   * Host callback: log message
   */
  host_log(msgPtr, len) {
    const msg = this.module.UTF8ToString(msgPtr, len);
    console.log('[Motus]', msg);
  }

  /**
   * Host callback: render complete
   */
  host_render_complete() {
    if (this.onComplete) {
      this.onComplete(this.output.join(''));
    }
  }

  /**
   * Host callback: begin dependency region
   */
  host_dep_start(pathPtr, len) {
    const path = this.module.UTF8ToString(pathPtr, len);
    this.currentDeps.push(path);
  }

  /**
   * Host callback: end dependency region
   */
  host_dep_end() {
    if (this.currentDeps.length > 0) {
      this.dependencies.push([...this.currentDeps]);
    }
    this.currentDeps = [];
  }

  /**
   * Host callback: static component boundary start (debug hook)
   */
  host_component_start(_funcIdx) {}

  /**
   * Host callback: static component boundary end (debug hook)
   */
  host_component_end(_funcIdx) {}

  /**
   * Host callback: slot default output start (debug hook)
   */
  host_slot_default_start(_funcIdx) {}

  /**
   * Host callback: slot default output end (debug hook)
   */
  host_slot_default_end(_funcIdx) {}

  /**
   * Host callback: VM debug step hook
   */
  host_debug_step(_chunkKind, _chunkIndex, _pc, _opcode, _line, _column, _sourcePtr, _sourceLen) {}

  /**
   * Host callback: load dynamic component from origin (disabled by default)
   */
  host_load_component(_reqId, _namePtr, _pathPtr, _propsPtr, _childrenPtr) {}

  /**
   * Host callback: load linked component from origin (disabled by default)
   */
  host_load_component_linked(_reqId, _namePtr, _pathPtr, _propsPtr, _childrenPtr) {}

  /**
   * Reset state for new render
   */
  reset() {
    this.output = [];
    this.dependencies = [];
    this.currentDeps = [];
    this.pendingFetches.clear();
  }

  /**
   * Get collected output as string
   */
  getOutput() {
    return this.output.join('');
  }

  /**
   * Get tracked dependencies
   */
  getDependencies() {
    return this.dependencies;
  }
}

/**
 * Create and initialize a Motus runtime instance
 */
async function createMotus(wasmPath = './mot-runtime.js') {
  // Load the Emscripten-generated module
  const MotRuntime = await import(wasmPath);

  const host = new MotusHost();

  // Create the module with imported host functions
  const module = await MotRuntime.default({
    env: {
      host_output: host.host_output.bind(host),
      host_output_text: host.host_output_text.bind(host),
      host_fetch_data: host.host_fetch_data.bind(host),
      host_error: host.host_error.bind(host),
      host_log: host.host_log.bind(host),
      host_render_complete: host.host_render_complete.bind(host),
      host_dep_start: host.host_dep_start.bind(host),
      host_dep_end: host.host_dep_end.bind(host),
      host_component_start: host.host_component_start.bind(host),
      host_component_end: host.host_component_end.bind(host),
      host_slot_default_start: host.host_slot_default_start.bind(host),
      host_slot_default_end: host.host_slot_default_end.bind(host),
      host_debug_step: host.host_debug_step.bind(host),
      host_load_component: host.host_load_component.bind(host),
      host_load_component_linked: host.host_load_component_linked.bind(host),
    }
  });

  host.setModule(module);

  return {
    /**
     * Render bytecode to HTML
     * @param {Uint8Array} bytecode - Compiled Motus bytecode
     * @returns {Promise<string>} - Rendered HTML
     */
    async render(bytecode) {
      host.reset();

      // Allocate memory for bytecode
      const ptr = module._malloc(bytecode.length);
      module.HEAPU8.set(bytecode, ptr);

      try {
        // Initialize runtime
        const initResult = module._mot_init(ptr, bytecode.length);
        if (initResult !== 0) {
          throw new Error('Failed to initialize runtime');
        }

        // Render
        const renderResult = module._mot_render();
        if (renderResult !== 0 && renderResult !== -2) {
          throw new Error('Render failed');
        }

        // Handle async data fetches if needed
        if (renderResult === -2) {
          // Would need to wait for data and call _mot_resume
          console.warn('Async data fetching not yet implemented');
        }

        return host.getOutput();
      } finally {
        module._mot_free();
        module._free(ptr);
      }
    },

    /**
     * Set callback for streaming output
     * @param {Function} callback - Called with (chunk, isEscaped)
     */
    onOutput(callback) {
      host.onOutput = callback;
    },

    /**
     * Set callback for data fetching
     * @param {Function} callback - Called with (name, query), returns Promise<data>
     */
    onFetch(callback) {
      host.onFetch = callback;
    },

    /**
     * Get dependencies tracked during last render
     */
    getDependencies() {
      return host.getDependencies();
    },

    /**
     * Toggle debug step mode in wasm runtime.
     */
    setDebugStepMode(enabled) {
      if (module._mot_set_debug_step_mode) {
        module._mot_set_debug_step_mode(enabled ? 1 : 0);
      }
    },

    /**
     * Invalidate cached output for a dependency path
     */
    invalidate(depPath) {
      const ptr = module.stringToUTF8(depPath, module._malloc(depPath.length + 1), depPath.length + 1);
      module._mot_invalidate(ptr, depPath.length);
      module._free(ptr);
    }
  };
}

// Export for different module systems
if (typeof module !== 'undefined' && module.exports) {
  module.exports = { createMotus, MotusHost };
}
if (typeof window !== 'undefined') {
  window.createMotus = createMotus;
}
