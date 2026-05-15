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
#include "../runtime/vm.h"
#include "../compiler/bytecode.h"
#include "../util/arena.h"
#include "../schema/schema_reader.h"

typedef struct {
    const char *input_path;
    const char *target;
    const char *bytecode_out;
    const char *wasm_out;
    const char *map_out;
    const char *css_out;
    const char *js_out;
    const char *html_out;
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

/* HTML output capture buffer */
static char html_buf[256 * 1024];
static int html_len = 0;

static void html_capture(const char *data, uint32_t len, void *ud) {
    (void)ud;
    if (html_len + (int)len < (int)sizeof(html_buf) - 1) {
        memcpy(html_buf + html_len, data, len);
        html_len += (int)len;
        html_buf[html_len] = '\0';
    }
}

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
        "  --html-out <path>       Output rendered HTML path\n"
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

/* --- Schema auto-invocation --- */

/* Portable memmem for platforms that don't have it */
static const void *mot_memmem(const void *haystack, size_t hlen,
                              const void *needle, size_t nlen) {
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    if (nlen == 0) return haystack;
    if (nlen > hlen) return NULL;
    size_t limit = hlen - nlen;
    for (size_t i = 0; i <= limit; i++) {
        if (memcmp(h + i, n, nlen) == 0) return h + i;
    }
    return NULL;
}

/* Extract directory from a file path.  Returns malloc'd string. */
static char *dir_of(const char *path) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) return dup_cstr(".");
    size_t len = (size_t)(last_slash - path);
    char *dir = (char *)malloc(len + 1);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

/*
 * Scan source for <import schema "path"> directives.
 * Returns an array of malloc'd path strings.  *out_count is set to the count.
 * Paths are resolved relative to input_dir.
 */
static char **scan_schema_imports(const char *source, size_t source_len,
                                  const char *input_dir, int *out_count) {
    int cap = 4, count = 0;
    char **paths = (char **)malloc(cap * sizeof(char *));
    if (!paths) { *out_count = 0; return NULL; }

    const char *p = source;
    const char *end = source + source_len;

    while (p < end) {
        /* Look for <import */
        const char *found = (const char *)mot_memmem(p, (size_t)(end - p), "<import", 7);
        if (!found) break;
        const char *after = found + 7;
        /* Skip whitespace */
        while (after < end && isspace((unsigned char)*after)) after++;
        /* Check for "schema" keyword */
        if (after + 6 <= end && strncmp(after, "schema", 6) == 0 &&
            (after + 6 >= end || isspace((unsigned char)after[6]) || after[6] == '"')) {
            after += 6;
            while (after < end && isspace((unsigned char)*after)) after++;
            /* Extract quoted path */
            if (after < end && *after == '"') {
                after++;
                const char *path_start = after;
                while (after < end && *after != '"') after++;
                if (after < end) {
                    size_t path_len = (size_t)(after - path_start);
                    /* Resolve relative to input dir */
                    size_t dir_len = strlen(input_dir);
                    char *full_path = (char *)malloc(dir_len + 1 + path_len + 1);
                    if (full_path) {
                        memcpy(full_path, input_dir, dir_len);
                        full_path[dir_len] = '/';
                        memcpy(full_path + dir_len + 1, path_start, path_len);
                        full_path[dir_len + 1 + path_len] = '\0';

                        if (count >= cap) {
                            cap *= 2;
                            char **tmp = (char **)realloc(paths, cap * sizeof(char *));
                            if (!tmp) { free(full_path); break; }
                            paths = tmp;
                        }
                        paths[count++] = full_path;
                    }
                }
            }
        }
        p = found + 1;
    }

    *out_count = count;
    return paths;
}

/*
 * Run capnp compile with capnpc-mot plugin on a schema file.
 * Returns the JSON output as a malloc'd string (NULL on failure).
 */
static char *invoke_capnp_compile(const char *schema_path, size_t *out_len) {
    /* Build command: capnp compile -o capnpc-mot schema.capnp */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "capnp compile -o capnpc-mot '%s' 2>/dev/null", schema_path);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "Failed to run capnp compile for %s\n", schema_path);
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { pclose(pipe); return NULL; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, pipe)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); pclose(pipe); return NULL; }
            buf = tmp;
        }
    }
    buf[len] = '\0';

    int status = pclose(pipe);
    if (status != 0 || len == 0) {
        fprintf(stderr, "capnp compile failed for %s (status=%d)\n", schema_path, status);
        free(buf);
        return NULL;
    }

    *out_len = len;
    return buf;
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
        } else if (strcmp(arg, "--html-out") == 0 && i + 1 < argc) {
            opts->html_out = argv[++i];
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
    compile_opts.schemas = NULL;
    compile_opts.schema_count = 0;

    /* Auto-detect and compile schema imports */
    {
        char *input_dir = dir_of(opts.input_path);
        int schema_path_count = 0;
        char **schema_paths = scan_schema_imports(source, source_len, input_dir,
                                                  &schema_path_count);
        if (schema_path_count > 0) {
            Arena *schema_arena = arena_create(16 * 1024);
            struct SchemaFile **schema_files = (struct SchemaFile **)malloc(
                (size_t)schema_path_count * sizeof(struct SchemaFile *));
            uint32_t loaded = 0;

            for (int si = 0; si < schema_path_count; si++) {
                size_t json_len = 0;
                char *json = invoke_capnp_compile(schema_paths[si], &json_len);
                if (json) {
                    SchemaFile *sf = schema_read_json(json, json_len, schema_arena);
                    if (sf) {
                        schema_files[loaded++] = sf;
                    } else {
                        fprintf(stderr, "Failed to parse schema JSON for %s\n",
                                schema_paths[si]);
                    }
                    free(json);
                }
                free(schema_paths[si]);
            }
            free(schema_paths);

            if (loaded > 0) {
                compile_opts.schemas = schema_files;
                compile_opts.schema_count = loaded;
            } else {
                free(schema_files);
                arena_destroy(schema_arena);
            }
        }
        free(input_dir);
    }
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

    /* HTML rendering: compile + execute in one pass */
    if (opts.html_out) {
        Arena *vm_arena = arena_create(64 * 1024);
        if (!vm_arena) {
            fprintf(stderr, "Failed to create VM arena\n");
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
        BytecodeModule *vm_mod = bytecode_deserialize(
            result.bytecode, (uint32_t)result.bytecode_len, vm_arena);
        if (!vm_mod) {
            fprintf(stderr, "Failed to deserialize bytecode for HTML rendering\n");
            arena_destroy(vm_arena);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }

        VM *vm = vm_new(vm_arena);
        vm_init(vm, vm_mod);
        html_len = 0;
        vm_set_output(vm, html_capture, NULL);

        VMResult vr = vm_run(vm);
        if (vr != VM_OK) {
            fprintf(stderr, "VM error: %s\n", vm_error_message(vm));
            arena_destroy(vm_arena);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }

        if (!write_file(opts.html_out, html_buf, (size_t)html_len)) {
            fprintf(stderr, "Failed to write HTML output: %s\n", opts.html_out);
            arena_destroy(vm_arena);
            mot_result_free(&result);
            free_linked_registry(&linked_registry);
            return 1;
        }
        arena_destroy(vm_arena);
    }

    mot_result_free(&result);
    free_linked_registry(&linked_registry);
    free(compile_opts.schemas);
    return 0;
}
