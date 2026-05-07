/*
 * Host Interface for WASM Runtime
 *
 * These functions must be provided by the host environment (JavaScript).
 * They are imported into the WASM module and called during execution.
 */

#ifndef MOT_WASM_HOST_H
#define MOT_WASM_HOST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#define WASM_IMPORT extern
#else
#define WASM_EXPORT
#define WASM_IMPORT
#endif

/*
 * Host-provided functions (imported from JavaScript)
 */

/* Output HTML content */
WASM_IMPORT void host_output(const char *data, uint32_t len);

/* Output escaped text */
WASM_IMPORT void host_output_text(const char *data, uint32_t len);

/* Request data fetch (async, query reference based) */
WASM_IMPORT void host_fetch_data(uint32_t req_id, uint32_t query_ref, uint32_t signature,
                                 const char *name, const char *params_json,
                                 uint32_t is_single);

/* Load dynamic component render via host/origin (async). */
WASM_IMPORT void host_load_component(uint32_t req_id, const char *name,
                                     const char *path, const char *props_json,
                                     const char *children_json);

/* Load linked component render via host/origin (async). */
WASM_IMPORT void host_load_component_linked(uint32_t req_id, const char *name,
                                            const char *path, const char *props_json,
                                            const char *children_json);

/* Report an error */
WASM_IMPORT void host_error(const char *msg, uint32_t len);

/* Log for debugging */
WASM_IMPORT void host_log(const char *msg, uint32_t len);

/* Notify render complete */
WASM_IMPORT void host_render_complete(void);

/* Begin dependency tracking region */
WASM_IMPORT void host_dep_start(const char *path, uint32_t len);

/* End dependency tracking region */
WASM_IMPORT void host_dep_end(void);

/* Debug VM step hook (chunk/opcode/source location) */
WASM_IMPORT void host_debug_step(uint32_t chunk_kind, uint32_t chunk_index, uint32_t pc,
                                 uint32_t opcode, uint32_t line, uint32_t column,
                                 const char *source_path, uint32_t source_len);

/*
 * WASM-exported functions (called from JavaScript)
 */

/* Initialize the runtime with bytecode */
WASM_EXPORT int mot_init(const uint8_t *bytecode, uint32_t len);

/* Run the render (may pause for async data) */
WASM_EXPORT int mot_render(void);

/* Execute a single VM instruction (may pause for async data) */
WASM_EXPORT int mot_step(void);

/* Run a component function render with props/children JSON payloads */
WASM_EXPORT int mot_render_component(uint32_t func_idx,
                                     const char *props_json, uint32_t props_len,
                                     const char *children_json, uint32_t children_len);

/* Resume render after data fetch */
WASM_EXPORT int mot_resume(uint32_t req_id, const char *json_data, uint32_t len);

/* Resume and execute a single VM instruction */
WASM_EXPORT int mot_resume_step(uint32_t req_id, const char *json_data, uint32_t len);

/* Invalidate cached output for a dependency */
WASM_EXPORT void mot_invalidate(const char *dep_path, uint32_t len);

/* Get current render state */
WASM_EXPORT int mot_state(void);

/* Build/read debug snapshot JSON */
WASM_EXPORT uint32_t mot_debug_snapshot_ptr(void);
WASM_EXPORT uint32_t mot_debug_snapshot_len(void);

/* Enable/disable debug step callbacks */
WASM_EXPORT void mot_set_debug_step_mode(uint32_t enabled);

/* Free runtime resources */
WASM_EXPORT void mot_free(void);

/* Render states */
#define MOT_STATE_IDLE       0
#define MOT_STATE_RUNNING    1
#define MOT_STATE_AWAITING   2  /* Waiting for data */
#define MOT_STATE_DONE       3
#define MOT_STATE_ERROR      4

/* Result codes */
#define MOT_OK               0
#define MOT_ERROR           -1
#define MOT_AWAIT           -2  /* Need to wait for data */

#endif /* MOT_WASM_HOST_H */
