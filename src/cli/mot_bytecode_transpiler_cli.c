/*
 * Motus bytecode->WASM transpiler CLI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../transpiler/bytecode_to_wasm.h"

typedef struct {
    const char *input_path;
    const char *output_path;
    const char *component_path;
    bool experimental;
} CliOptions;

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s --input <bytecode.bin> --output <component.wasm> [options]\n"
        "\n"
        "Options:\n"
        "  --input <path>             Input bytecode file (required)\n"
        "  --output <path>            Output wasm file (required)\n"
        "  --component-path <path>    Logical component path (diagnostics)\n"
        "  --experimental             Enable experimental lowering paths\n"
        "  --help                     Show this message\n",
        argv0);
}

static int read_file(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *f;
    long size;
    size_t n;
    uint8_t *buf;

    if (!path || !out_data || !out_len) return 0;
    *out_data = NULL;
    *out_len = 0;

    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    buf = (uint8_t *)malloc((size_t)size);
    if (!buf && size != 0) {
        fclose(f);
        return 0;
    }

    n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return 0;
    }

    *out_data = buf;
    *out_len = (size_t)size;
    return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f;
    size_t n;

    if (!path || (!data && len > 0)) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static int parse_args(int argc, char **argv, CliOptions *opts) {
    int i;
    memset(opts, 0, sizeof(*opts));
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--input") == 0 && i + 1 < argc) {
            opts->input_path = argv[++i];
        } else if (strcmp(arg, "--output") == 0 && i + 1 < argc) {
            opts->output_path = argv[++i];
        } else if (strcmp(arg, "--component-path") == 0 && i + 1 < argc) {
            opts->component_path = argv[++i];
        } else if (strcmp(arg, "--experimental") == 0) {
            opts->experimental = true;
        } else {
            fprintf(stderr, "Unknown/invalid option: %s\n", arg);
            usage(argv[0]);
            return -1;
        }
    }

    if (!opts->input_path || !opts->output_path) {
        fprintf(stderr, "Missing required --input/--output\n");
        usage(argv[0]);
        return -1;
    }
    return 1;
}

int main(int argc, char **argv) {
    CliOptions opts;
    int parse_rc;
    uint8_t *bytecode = NULL;
    size_t bytecode_len = 0;
    uint8_t *wasm = NULL;
    size_t wasm_len = 0;
    MotBytecodeToWasmOptions transpile_opts;
    MotTranspilerStatus status;
    char error_buf[256];

    parse_rc = parse_args(argc, argv, &opts);
    if (parse_rc <= 0) return parse_rc == 0 ? 0 : 1;

    if (!read_file(opts.input_path, &bytecode, &bytecode_len)) {
        fprintf(stderr, "Failed to read input file: %s\n", opts.input_path);
        return 1;
    }

    transpile_opts.component_path = opts.component_path;
    transpile_opts.enable_experimental_lowering = opts.experimental;
    status = mot_transpile_component_bytecode_to_wasm(
        bytecode,
        bytecode_len,
        &transpile_opts,
        &wasm,
        &wasm_len,
        error_buf,
        sizeof(error_buf)
    );
    free(bytecode);

    if (status == MOT_TRANSPILER_OK) {
        if (!wasm || wasm_len == 0) {
            free(wasm);
            fprintf(stderr, "Transpiler returned success but no wasm output\n");
            return 1;
        }
        if (!write_file(opts.output_path, wasm, wasm_len)) {
            free(wasm);
            fprintf(stderr, "Failed to write output file: %s\n", opts.output_path);
            return 1;
        }
        free(wasm);
        return 0;
    }

    free(wasm);
    fprintf(stderr, "%s\n", error_buf[0] ? error_buf : "transpiler failed");
    return 1;
}
