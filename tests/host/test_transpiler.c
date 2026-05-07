#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../../src/mot.h"
#include "../../src/compiler/bytecode.h"

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

int main(void) {
    uint8_t module[46];
    uint8_t *wasm = NULL;
    size_t wasm_len = 0;
    char err[256];
    MotTranspilerStatus status;

    /* Minimal valid module:
     * header + counts + empty main chunk + one empty function chunk.
     */
    memset(module, 0, sizeof(module));
    write_u32_le(module + 0, BYTECODE_MAGIC);
    write_u16_le(module + 4, BYTECODE_VERSION_MAJOR);
    write_u16_le(module + 6, BYTECODE_VERSION_MINOR);
    write_u16_le(module + 8, 0);
    write_u32_le(module + 34, 1); /* func_count */
    write_u32_le(module + 38, 0); /* main_len */
    write_u32_le(module + 42, 0); /* function[0]_len */

    status = mot_transpile_component_bytecode_to_wasm(
        module,
        sizeof(module),
        NULL,
        &wasm,
        &wasm_len,
        err,
        sizeof(err)
    );
    if (status != MOT_TRANSPILER_OK) {
        fprintf(stderr, "Expected OK for valid header, got %d\n", status);
        return 1;
    }
    if (wasm == NULL || wasm_len < 8) {
        fprintf(stderr, "Expected wasm output for valid input\n");
        return 1;
    }
    if (!(wasm[0] == 0x00 && wasm[1] == 0x61 && wasm[2] == 0x73 && wasm[3] == 0x6d)) {
        fprintf(stderr, "Expected wasm magic in transpiler output\n");
        free(wasm);
        return 1;
    }
    free(wasm);
    wasm = NULL;
    wasm_len = 0;

    module[0] ^= 0x01;
    status = mot_transpile_component_bytecode_to_wasm(
        module,
        sizeof(module),
        NULL,
        &wasm,
        &wasm_len,
        err,
        sizeof(err)
    );
    if (status != MOT_TRANSPILER_INVALID_INPUT) {
        fprintf(stderr, "Expected INVALID_INPUT for bad magic, got %d\n", status);
        return 1;
    }

    printf("transpiler API tests passed\n");
    return 0;
}
