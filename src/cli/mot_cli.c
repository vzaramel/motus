/*
 * Motus CLI
 *
 * Minimal compiler front-end with target selection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "../mot.h"

typedef struct {
    const char *input_path;
    const char *target;
    const char *bytecode_out;
    const char *wasm_out;
    const char *map_out;
    const char *css_out;
    const char *js_out;
    const char *linked_manifest;
    bool partial_eval;
    bool include_debug;
} CliOptions;

typedef struct LinkedPathEntry {
    char *path;
    struct LinkedPathEntry *next;
} LinkedPathEntry;

typedef struct {
    LinkedPathEntry *head;
} LinkedPathRegistry;

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --input <path>          Input .mot file (required)\n"
        "  --target <mode>         bytecode | wasm | both (default: bytecode)\n"
        "  --bytecode-out <path>   Output bytecode path\n"
        "  --wasm-out <path>       Output wasm path\n"
        "  --map-out <path>        Output source map JSON path\n"
        "  --css-out <path>        Output extracted css path\n"
        "  --js-out <path>         Output extracted js path\n"
        "  --linked-manifest <p>   Manifest of linked component paths\n"
        "  --no-partial-eval       Disable partial evaluation\n"
        "  --no-debug              Disable debug metadata\n"
        "  --help                  Show this message\n",
        argv0);
}

static char *dup_cstr(const char *s) {
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s);
    d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static void normalize_component_path(const char *input, char *out, size_t out_cap) {
    const char *src = input ? input : "";
    size_t len = 0;
    if (!out || out_cap == 0) return;
    while (*src && isspace((unsigned char)*src)) src++;
    while (src[0] == '.' && src[1] == '/') src += 2;
    len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    if (len >= 4 && strncmp(src + len - 4, ".mot", 4) == 0) len -= 4;
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

static bool linked_registry_has(const LinkedPathRegistry *registry, const char *path) {
    const LinkedPathEntry *it = registry ? registry->head : NULL;
    while (it) {
        if (strcmp(it->path, path) == 0) return true;
        it = it->next;
    }
    return false;
}

static void linked_registry_add(LinkedPathRegistry *registry, const char *raw_path) {
    char normalized[512];
    LinkedPathEntry *entry;
    if (!registry || !raw_path || !*raw_path) return;
    normalize_component_path(raw_path, normalized, sizeof(normalized));
    if (!normalized[0]) return;
    if (linked_registry_has(registry, normalized)) return;
    entry = (LinkedPathEntry *)calloc(1, sizeof(LinkedPathEntry));
    if (!entry) return;
    entry->path = dup_cstr(normalized);
    if (!entry->path) {
        free(entry);
        return;
    }
    entry->next = registry->head;
    registry->head = entry;
}

static bool load_linked_manifest(LinkedPathRegistry *registry, const char *manifest_path) {
    FILE *f;
    char line[1024];
    if (!registry || !manifest_path) return false;
    f = fopen(manifest_path, "r");
    if (!f) return false;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) p[--n] = '\0';
        if (n == 0) continue;
        linked_registry_add(registry, p);
    }
    fclose(f);
    return true;
}

static bool linked_manifest_resolver(const char *path, void *userdata) {
    char normalized[512];
    const LinkedPathRegistry *registry = (const LinkedPathRegistry *)userdata;
    if (!registry || !path) return false;
    normalize_component_path(path, normalized, sizeof(normalized));
    if (!normalized[0]) return false;
    return linked_registry_has(registry, normalized);
}

static void free_linked_registry(LinkedPathRegistry *registry) {
    LinkedPathEntry *it;
    if (!registry) return;
    it = registry->head;
    while (it) {
        LinkedPathEntry *next = it->next;
        free(it->path);
        free(it);
        it = next;
    }
    registry->head = NULL;
}

static int read_file(const char *path, char **out_data, size_t *out_len) {
    FILE *f;
    long size;
    size_t read_n;
    char *buf;

    if (!path || !out_data || !out_len) return 0;

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

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }

    read_n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read_n != (size_t)size) {
        free(buf);
        return 0;
    }

    buf[size] = '\0';
    *out_data = buf;
    *out_len = (size_t)size;
    return 1;
}

static int write_file(const char *path, const void *data, size_t len) {
    FILE *f;
    size_t n;

    if (!path) return 1; /* Optional output */
    if (!data && len != 0) return 0;

    f = fopen(path, "wb");
    if (!f) return 0;
    n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static MotCompileTarget parse_target(const char *target) {
    if (!target || strcmp(target, "bytecode") == 0) return MOT_TARGET_BYTECODE;
    if (strcmp(target, "wasm") == 0) return MOT_TARGET_WASM;
    if (strcmp(target, "both") == 0) return MOT_TARGET_BOTH;
    return MOT_TARGET_BYTECODE;
}

static int parse_args(int argc, char **argv, CliOptions *opts) {
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->target = "bytecode";
    opts->partial_eval = true;
    opts->include_debug = true;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(arg, "--input") == 0 && i + 1 < argc) {
            opts->input_path = argv[++i];
        } else if (strcmp(arg, "--target") == 0 && i + 1 < argc) {
            opts->target = argv[++i];
        } else if (strcmp(arg, "--bytecode-out") == 0 && i + 1 < argc) {
            opts->bytecode_out = argv[++i];
        } else if (strcmp(arg, "--wasm-out") == 0 && i + 1 < argc) {
            opts->wasm_out = argv[++i];
        } else if (strcmp(arg, "--map-out") == 0 && i + 1 < argc) {
            opts->map_out = argv[++i];
        } else if (strcmp(arg, "--css-out") == 0 && i + 1 < argc) {
            opts->css_out = argv[++i];
        } else if (strcmp(arg, "--js-out") == 0 && i + 1 < argc) {
            opts->js_out = argv[++i];
        } else if (strcmp(arg, "--linked-manifest") == 0 && i + 1 < argc) {
            opts->linked_manifest = argv[++i];
        } else if (strcmp(arg, "--no-partial-eval") == 0) {
            opts->partial_eval = false;
        } else if (strcmp(arg, "--no-debug") == 0) {
            opts->include_debug = false;
        } else {
            fprintf(stderr, "Unknown/invalid option: %s\n", arg);
            usage(argv[0]);
            return -1;
        }
    }

    if (!opts->input_path) {
        fprintf(stderr, "Missing required --input\n");
        usage(argv[0]);
        return -1;
    }

    if (strcmp(opts->target, "bytecode") != 0 &&
        strcmp(opts->target, "wasm") != 0 &&
        strcmp(opts->target, "both") != 0) {
        fprintf(stderr, "Invalid --target: %s (expected bytecode|wasm|both)\n", opts->target);
        return -1;
    }

    return 1;
}

int main(int argc, char **argv) {
    CliOptions opts;
    int parse_rc;
    char *source = NULL;
    size_t source_len = 0;
    MotCompileOptions compile_opts;
    MotCompileResult result;
    size_t i;
    LinkedPathRegistry linked_registry = {0};
    bool linked_manifest_loaded = false;

    parse_rc = parse_args(argc, argv, &opts);
    if (parse_rc <= 0) {
        return parse_rc == 0 ? 0 : 1;
    }

    if (!read_file(opts.input_path, &source, &source_len)) {
        fprintf(stderr, "Failed to read input file: %s\n", opts.input_path);
        return 1;
    }

    compile_opts.target = parse_target(opts.target);
    compile_opts.partial_eval = opts.partial_eval;
    compile_opts.include_debug = opts.include_debug;
    if (opts.linked_manifest) {
        linked_manifest_loaded = load_linked_manifest(&linked_registry, opts.linked_manifest);
        if (!linked_manifest_loaded) {
            fprintf(stderr, "Failed to read linked manifest: %s\n", opts.linked_manifest);
            free(source);
            return 1;
        }
        compile_opts.linked_component_resolver = linked_manifest_resolver;
        compile_opts.linked_component_userdata = &linked_registry;
    } else {
        compile_opts.linked_component_resolver = NULL;
        compile_opts.linked_component_userdata = NULL;
    }

    result = mot_compile_with_options(source, source_len, &compile_opts);
    free(source);

    if (result.errors.count > 0) {
        for (i = 0; i < result.errors.count; i++) {
            const MotError *err = &result.errors.errors[i];
            fprintf(stderr, "[error] %d:%d %s\n", err->line, err->column, err->message);
        }
        mot_result_free(&result);
        free_linked_registry(&linked_registry);
        return 1;
    }

    if ((compile_opts.target == MOT_TARGET_BYTECODE || compile_opts.target == MOT_TARGET_BOTH) &&
        result.bytecode && result.bytecode_len > 0 && opts.bytecode_out) {
        if (!write_file(opts.bytecode_out, result.bytecode, result.bytecode_len)) {
            fprintf(stderr, "Failed to write bytecode output: %s\n", opts.bytecode_out);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
    }

    if ((compile_opts.target == MOT_TARGET_WASM || compile_opts.target == MOT_TARGET_BOTH) &&
        result.wasm && result.wasm_len > 0 && opts.wasm_out) {
        if (!write_file(opts.wasm_out, result.wasm, result.wasm_len)) {
            fprintf(stderr, "Failed to write wasm output: %s\n", opts.wasm_out);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
    }

    if (result.source_map_json && opts.map_out) {
        if (!write_file(opts.map_out, result.source_map_json, strlen(result.source_map_json))) {
            fprintf(stderr, "Failed to write source map output: %s\n", opts.map_out);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
    }

    if (result.css && opts.css_out) {
        if (!write_file(opts.css_out, result.css, strlen(result.css))) {
            fprintf(stderr, "Failed to write css output: %s\n", opts.css_out);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
    }

    if (result.js && opts.js_out) {
        if (!write_file(opts.js_out, result.js, strlen(result.js))) {
            fprintf(stderr, "Failed to write js output: %s\n", opts.js_out);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
    }

    mot_result_free(&result);
    free_linked_registry(&linked_registry);
    return 0;
}
