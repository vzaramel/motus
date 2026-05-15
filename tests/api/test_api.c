/*
 * Public API tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mot.h"
#include "compiler/bytecode.h"
#include "runtime/vm.h"
#include "util/arena.h"

static int tests_run = 0;
static int tests_passed = 0;
static int test_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s...", #name); \
    fflush(stdout); \
    tests_run++; \
    test_failed = 0; \
    test_##name(); \
    if (!test_failed) { \
        tests_passed++; \
        printf(" PASSED\n"); \
    } \
    fflush(stdout); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf(" FAILED: %s\n", msg); \
        test_failed = 1; \
        return; \
    } \
} while(0)

static char output_buffer[1024];
static int output_len = 0;

static void capture_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    if (output_len + (int)len < (int)sizeof(output_buffer) - 1) {
        memcpy(output_buffer + output_len, data, len);
        output_len += (int)len;
        output_buffer[output_len] = '\0';
    }
}

static int contains_bytes(const uint8_t *haystack, size_t haystack_len,
                          const char *needle) {
    size_t needle_len = strlen(needle);
    if (!haystack || !needle || needle_len == 0 || needle_len > haystack_len) return 0;

    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int read_u8_at(const uint8_t *data, size_t len, size_t *pos, uint8_t *out) {
    if (*pos + 1 > len) return 0;
    *out = data[(*pos)++];
    return 1;
}

static int read_u16_at(const uint8_t *data, size_t len, size_t *pos, uint16_t *out) {
    if (*pos + 2 > len) return 0;
    *out = (uint16_t)(data[*pos] | (data[*pos + 1] << 8));
    *pos += 2;
    return 1;
}

static int read_u32_at(const uint8_t *data, size_t len, size_t *pos, uint32_t *out) {
    if (*pos + 4 > len) return 0;
    *out = (uint32_t)(data[*pos] |
                      (data[*pos + 1] << 8) |
                      (data[*pos + 2] << 16) |
                      (data[*pos + 3] << 24));
    *pos += 4;
    return 1;
}

static int skip_bytes(size_t len, size_t *pos, size_t count) {
    if (*pos + count > len) return 0;
    *pos += count;
    return 1;
}

static int extract_debug_query_info(const uint8_t *data, size_t len,
                                    uint32_t *out_query_count,
                                    uint32_t *out_first_query_line) {
    size_t pos = 0;
    uint32_t const_count = 0, string_count = 0, data_req_count = 0;
    uint32_t dep_count = 0, builtin_count = 0, comp_ref_count = 0;
    uint32_t mut_req_count = 0, func_count = 0;
    uint32_t tmp_u32 = 0;
    uint16_t tmp_u16 = 0;
    uint8_t tmp_u8 = 0;

    /* Header */
    if (!read_u32_at(data, len, &pos, &tmp_u32)) return 0; /* magic */
    if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* major */
    if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* minor */
    if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* flags */

    if (!read_u32_at(data, len, &pos, &const_count) ||
        !read_u32_at(data, len, &pos, &string_count) ||
        !read_u32_at(data, len, &pos, &data_req_count) ||
        !read_u32_at(data, len, &pos, &dep_count) ||
        !read_u32_at(data, len, &pos, &builtin_count) ||
        !read_u32_at(data, len, &pos, &comp_ref_count) ||
        !read_u32_at(data, len, &pos, &mut_req_count) ||
        !read_u32_at(data, len, &pos, &func_count)) {
        return 0;
    }

    for (uint32_t i = 0; i < const_count; i++) {
        if (!read_u8_at(data, len, &pos, &tmp_u8)) return 0;
        if (tmp_u8 == 0 || tmp_u8 == 1) {
            if (!skip_bytes(len, &pos, 1)) return 0;
        } else if (tmp_u8 == 2 || tmp_u8 == 3) {
            if (!skip_bytes(len, &pos, 8)) return 0;
        } else if (tmp_u8 == 4) {
            if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    for (uint32_t i = 0; i < string_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    }

    for (uint32_t i = 0; i < data_req_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
        if (!skip_bytes(len, &pos, 2)) return 0; /* is_single/is_dynamic */
        if (!skip_bytes(len, &pos, 4 + 4)) return 0; /* query_ref/signature */
        if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* param count */
        for (uint16_t p = 0; p < tmp_u16; p++) {
            if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
            if (!skip_bytes(len, &pos, 2)) return 0; /* param slot */
        }
    }

    for (uint32_t i = 0; i < dep_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    }

    for (uint32_t i = 0; i < builtin_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
        if (!skip_bytes(len, &pos, 2)) return 0;
    }

    for (uint32_t i = 0; i < comp_ref_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    }

    for (uint32_t i = 0; i < mut_req_count; i++) {
        if (!skip_bytes(len, &pos, 1)) return 0; /* type */
        if (!skip_bytes(len, &pos, 1)) return 0; /* optimistic */
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0; /* target */
        if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* field_count */
        for (uint16_t f = 0; f < tmp_u16; f++) {
            if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
        }
    }

    if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0; /* main */
    for (uint32_t i = 0; i < func_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    }

    if (!read_u32_at(data, len, &pos, &tmp_u32)) return 0;
    if (tmp_u32 != BYTECODE_DEBUG_MAGIC) return 0;

    if (!read_u32_at(data, len, &pos, &tmp_u32)) return 0; /* span count */
    for (uint32_t i = 0; i < tmp_u32; i++) {
        if (!skip_bytes(len, &pos, 1 + 2 + 4 + 4 + 4 + 4 + 2)) return 0;
    }

    if (!read_u32_at(data, len, &pos, out_query_count)) return 0;
    for (uint32_t i = 0; i < *out_query_count; i++) {
        uint32_t query_ref = 0, signature = 0, line = 0, column = 0;
        uint16_t name_len = 0;
        if (!read_u32_at(data, len, &pos, &query_ref) ||
            !read_u32_at(data, len, &pos, &signature) ||
            !read_u32_at(data, len, &pos, &line) ||
            !read_u32_at(data, len, &pos, &column) ||
            !read_u16_at(data, len, &pos, &name_len) ||
            !skip_bytes(len, &pos, name_len)) {
            return 0;
        }
        (void)query_ref;
        (void)signature;
        (void)column;
        if (i == 0 && out_first_query_line) {
            *out_first_query_line = line;
        }
    }

    if (pos < len) {
        uint32_t fn_ref_count = 0;
        if (!read_u32_at(data, len, &pos, &fn_ref_count)) return 0;
        for (uint32_t i = 0; i < fn_ref_count; i++) {
            uint16_t func_idx = 0, name_len = 0, source_len = 0;
            if (!read_u16_at(data, len, &pos, &func_idx) ||
                !read_u16_at(data, len, &pos, &name_len) ||
                !skip_bytes(len, &pos, name_len) ||
                !read_u16_at(data, len, &pos, &source_len) ||
                !skip_bytes(len, &pos, source_len)) {
                return 0;
            }
            (void)func_idx;
        }
    }

    return pos == len;
}

TEST(compile_success) {
    const char *src = "<output \"Hello\">";
    MotCompileResult result = mot_compile(src, strlen(src));

    ASSERT(result.errors.count == 0, "Compilation should have no errors");
    ASSERT(result.bytecode != NULL, "Bytecode should be present");
    ASSERT(result.bytecode_len > 0, "Bytecode length should be > 0");
    ASSERT(result.css != NULL, "CSS should be allocated");
    ASSERT(result.js != NULL, "JS should be allocated");

    mot_result_free(&result);
}

TEST(compile_parse_error) {
    const char *src = "<output";
    MotCompileResult result = mot_compile(src, strlen(src));

    ASSERT(result.errors.count > 0, "Compilation should report errors");
    ASSERT(result.bytecode == NULL, "Bytecode should be absent on parse error");

    mot_result_free(&result);
}

TEST(parse_only) {
    const char *src = "<div>ok</div>";
    Arena *arena = arena_create(4096);

    MotError errs_storage[8];
    MotErrorList errs;
    errs.errors = errs_storage;
    errs.count = 0;
    errs.capacity = 8;

    struct AstNode *doc = mot_parse(src, strlen(src), arena, &errs);
    ASSERT(doc != NULL, "Parse API should return an AST");
    ASSERT(errs.count == 0, "Parse API should not report errors");

    arena_destroy(arena);
}

TEST(deserialize_roundtrip_and_run) {
    const char *src = "<output \"Hello\">";
    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Compile should succeed");

    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    ASSERT(mod != NULL, "Deserialize should succeed");
    ASSERT(mod->main.code_len > 0, "Deserialized module should have code");

    output_buffer[0] = '\0';
    output_len = 0;

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    vm_set_output(vm, capture_output, NULL);
    ASSERT(vm_run(vm) == VM_OK, "VM run should succeed");
    ASSERT(strcmp(output_buffer, "Hello") == 0, "Roundtrip bytecode should render output");

    arena_destroy(arena);
    mot_result_free(&result);
}

TEST(sql_query_not_embedded_in_serialized_bytecode) {
    const char *src =
        "<let productId 42>"
        "<let product select * from products where id = :productId><output productId></let>"
        "</let>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Compile should succeed");
    ASSERT(result.bytecode != NULL, "Bytecode should be present");
    ASSERT(result.bytecode_len > 0, "Bytecode length should be > 0");
    ASSERT(!contains_bytes(result.bytecode, result.bytecode_len, "products"),
           "Serialized bytecode should not embed SQL text");

    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    ASSERT(mod != NULL, "Deserialize should succeed");
    ASSERT(mod->data_req_count == 1, "Should have one data requirement");
    ASSERT(mod->data_reqs[0].query_len == 0, "Deserialized query text should be stripped");
    ASSERT(mod->data_reqs[0].signature != 0, "Signature metadata should be preserved");

    arena_destroy(arena);
    mot_result_free(&result);
}

TEST(deserialize_rejects_old_bytecode_version) {
    const char *src = "<output \"Hello\">";
    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Compile should succeed");
    ASSERT(result.bytecode != NULL, "Bytecode should be present");
    ASSERT(result.bytecode_len >= 10, "Bytecode should include header");

    /* Header: magic(4), version_major(2), version_minor(2), flags(2) */
    result.bytecode[6] = 1;  /* force version minor to 1 */
    result.bytecode[7] = 0;

    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    ASSERT(mod == NULL, "Deserializer should reject unsupported bytecode version");

    arena_destroy(arena);
    mot_result_free(&result);
}

TEST(serialized_includes_debug_trailer) {
    const char *src =
        "<let productId 7>"
        "<let product dynamic single select id, name from products where id = :productId>"
        "<output productId>"
        "</let>"
        "</let>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Compile should succeed");
    ASSERT(result.bytecode != NULL, "Bytecode should be present");
    ASSERT(result.bytecode_len > 0, "Bytecode length should be > 0");

    uint32_t query_count = 0;
    uint32_t first_query_line = 0;
    ASSERT(extract_debug_query_info(result.bytecode, result.bytecode_len, &query_count, &first_query_line),
           "Serialized bytecode should include parseable debug trailer");
    ASSERT(query_count == 1, "Debug trailer should contain one query info entry");
    ASSERT(first_query_line > 0, "Debug query info should include source line");

    mot_result_free(&result);
}

TEST(compile_with_options_bytecode_target) {
    const char *src = "<output \"Hello\">";
    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_BYTECODE;
    options.partial_eval = true;
    options.include_debug = true;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    ASSERT(result.errors.count == 0, "Bytecode target should compile cleanly");
    ASSERT(result.bytecode != NULL, "Bytecode target should produce bytecode output");
    ASSERT(result.bytecode_len > 0, "Bytecode output should have length");
    ASSERT(result.wasm == NULL, "Bytecode target should not produce wasm bytes");
    ASSERT(result.wasm_len == 0, "Bytecode target wasm length should be zero");

    mot_result_free(&result);
}

TEST(compile_with_options_wasm_target_not_implemented) {
    const char *src = "<output \"Hello\">";
    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_WASM;
    options.partial_eval = true;
    options.include_debug = true;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    ASSERT(result.errors.count > 0, "WASM target should currently report a clear error");
    ASSERT(result.bytecode == NULL, "WASM target should not emit bytecode in current core path");
    ASSERT(result.wasm == NULL, "WASM target should not emit wasm in current core path");

    mot_result_free(&result);
}

TEST(compile_with_options_both_target_not_implemented) {
    const char *src = "<output \"Hello\">";
    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_BOTH;
    options.partial_eval = true;
    options.include_debug = true;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    ASSERT(result.errors.count > 0, "BOTH target should currently report a clear error");
    ASSERT(result.bytecode == NULL, "BOTH target should not emit bytecode in current core path");
    ASSERT(result.wasm == NULL, "BOTH target should not emit wasm in current core path");

    mot_result_free(&result);
}

TEST(compile_with_options_disable_debug_trailer) {
    const char *src =
        "<let productId 7>"
        "<let product dynamic single select id, name from products where id = :productId>"
        "<output productId>"
        "</let>"
        "</let>";

    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_BYTECODE;
    options.partial_eval = true;
    options.include_debug = false;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    uint32_t query_count = 0;
    uint32_t first_query_line = 0;
    ASSERT(result.errors.count == 0, "Compile should succeed with include_debug disabled");
    ASSERT(result.bytecode != NULL, "Bytecode should be present");
    ASSERT(!extract_debug_query_info(result.bytecode, result.bytecode_len, &query_count, &first_query_line),
           "Serialized bytecode should not include debug trailer when include_debug=false");

    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    ASSERT(mod != NULL, "Deserializer should accept no-debug bytecode");
    ASSERT(mod->data_req_count == 1, "Bytecode content should remain valid");

    arena_destroy(arena);
    mot_result_free(&result);
}

/* ---- Module handle API tests ---- */

TEST(module_compile_to_module) {
    const char *src = "<output \"Hello\">";
    MotErrorList errors;
    memset(&errors, 0, sizeof(errors));

    MotModule *mod = mot_compile_to_module(src, strlen(src), NULL, &errors);
    ASSERT(mod != NULL, "mot_compile_to_module should succeed");
    ASSERT(errors.count == 0, "No errors expected");

    BytecodeModule *bc = mot_module_bytecode(mod);
    ASSERT(bc != NULL, "Module handle should expose BytecodeModule");
    ASSERT(bc->main.code_len > 0, "Module should have main code");

    mot_error_list_free(&errors);
    mot_module_free(mod);
}

TEST(module_compile_to_module_error) {
    const char *src = "<output";
    MotErrorList errors;
    memset(&errors, 0, sizeof(errors));

    MotModule *mod = mot_compile_to_module(src, strlen(src), NULL, &errors);
    ASSERT(mod == NULL, "mot_compile_to_module should fail on bad input");
    ASSERT(errors.count > 0, "Errors should be reported");

    mot_error_list_free(&errors);
}

TEST(module_compile_ast) {
    const char *src = "<output \"World\">";
    Arena *arena = arena_create(8192);

    MotErrorList parse_errs;
    parse_errs.count = 0;
    parse_errs.capacity = 16;
    parse_errs.errors = arena_alloc(arena, parse_errs.capacity * sizeof(MotError));

    struct AstNode *doc = mot_parse(src, strlen(src), arena, &parse_errs);
    ASSERT(doc != NULL, "Parse should succeed");

    MotErrorList errors;
    memset(&errors, 0, sizeof(errors));

    MotModule *mod = mot_compile_ast(arena, doc, NULL, &errors);
    ASSERT(mod != NULL, "mot_compile_ast should succeed");
    ASSERT(errors.count == 0, "No compile errors expected");

    BytecodeModule *bc = mot_module_bytecode(mod);
    ASSERT(bc != NULL, "Module should have bytecode");

    mot_error_list_free(&errors);
    mot_module_free(mod);  /* also frees the arena */
}

TEST(module_serialize_and_run) {
    const char *src = "<output \"Module\">";
    MotModule *mod = mot_compile_to_module(src, strlen(src), NULL, NULL);
    ASSERT(mod != NULL, "Compile to module should succeed");

    uint32_t len = 0;
    uint8_t *bytes = mot_module_serialize(mod, &len, false);
    ASSERT(bytes != NULL, "Serialize should return bytes");
    ASSERT(len > 0, "Serialized length should be > 0");

    /* Deserialize and run */
    Arena *arena = arena_create(8192);
    BytecodeModule *deserialized = bytecode_deserialize(bytes, len, arena);
    ASSERT(deserialized != NULL, "Deserialized module should be valid");

    output_buffer[0] = '\0';
    output_len = 0;

    VM *vm = vm_new(arena);
    vm_init(vm, deserialized);
    vm_set_output(vm, capture_output, NULL);
    ASSERT(vm_run(vm) == VM_OK, "VM should run successfully");
    ASSERT(strcmp(output_buffer, "Module") == 0, "Should render correct output");

    arena_destroy(arena);
    free(bytes);
    mot_module_free(mod);
}

TEST(module_null_safety) {
    /* All accessors should be safe with NULL */
    ASSERT(mot_module_bytecode(NULL) == NULL, "Bytecode from NULL should be NULL");

    uint32_t len = 99;
    ASSERT(mot_module_serialize(NULL, &len, false) == NULL, "Serialize NULL should return NULL");
    ASSERT(len == 0, "Serialize NULL should set length to 0");

    /* These should not crash */
    mot_module_free(NULL);
    mot_error_list_free(NULL);
}

TEST(module_compile_ast_failure_preserves_arena) {
    /* Compile an AST that will fail analysis (undefined variable) */
    const char *src = "<output undeclared_var>";
    Arena *arena = arena_create(8192);

    MotErrorList parse_errs;
    parse_errs.count = 0;
    parse_errs.capacity = 16;
    parse_errs.errors = arena_alloc(arena, parse_errs.capacity * sizeof(MotError));

    struct AstNode *doc = mot_parse(src, strlen(src), arena, &parse_errs);
    ASSERT(doc != NULL, "Parse should succeed (bad semantics, not syntax)");

    MotErrorList errors;
    memset(&errors, 0, sizeof(errors));

    MotModule *mod = mot_compile_ast(arena, doc, NULL, &errors);
    ASSERT(mod == NULL, "Compile should fail for undefined variable");
    ASSERT(errors.count > 0, "Errors should be reported");

    /* Arena should still be valid (caller still owns it on failure) */
    mot_error_list_free(&errors);
    arena_destroy(arena);  /* must not crash */
}

int main(void) {
    printf("=== API Tests ===\n");

    RUN_TEST(compile_success);
    RUN_TEST(compile_parse_error);
    RUN_TEST(parse_only);
    RUN_TEST(deserialize_roundtrip_and_run);
    RUN_TEST(sql_query_not_embedded_in_serialized_bytecode);
    RUN_TEST(deserialize_rejects_old_bytecode_version);
    RUN_TEST(serialized_includes_debug_trailer);
    RUN_TEST(compile_with_options_bytecode_target);
    RUN_TEST(compile_with_options_wasm_target_not_implemented);
    RUN_TEST(compile_with_options_both_target_not_implemented);
    RUN_TEST(compile_with_options_disable_debug_trailer);
    RUN_TEST(module_compile_to_module);
    RUN_TEST(module_compile_to_module_error);
    RUN_TEST(module_compile_ast);
    RUN_TEST(module_serialize_and_run);
    RUN_TEST(module_null_safety);
    RUN_TEST(module_compile_ast_failure_preserves_arena);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
