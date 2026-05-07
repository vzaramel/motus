/*
 * Bytecode -> WASM transpiler interface (core C implementation).
 */

#ifndef MOT_BYTECODE_TO_WASM_H
#define MOT_BYTECODE_TO_WASM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MOT_TRANSPILER_OK = 0,
    MOT_TRANSPILER_INVALID_INPUT = 1,
    MOT_TRANSPILER_OOM = 2,
    MOT_TRANSPILER_INTERNAL_ERROR = 3,
} MotTranspilerStatus;

typedef struct {
    const char *component_path;
    bool enable_experimental_lowering;
} MotBytecodeToWasmOptions;

MotTranspilerStatus mot_transpile_component_bytecode_to_wasm(
    const uint8_t *bytecode,
    size_t bytecode_len,
    const MotBytecodeToWasmOptions *options,
    uint8_t **out_wasm,
    size_t *out_wasm_len,
    char *error_buf,
    size_t error_buf_len
);

#endif /* MOT_BYTECODE_TO_WASM_H */
