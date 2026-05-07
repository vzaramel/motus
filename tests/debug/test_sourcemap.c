/*
 * Source map helper tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/sourcemap.h"

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

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static void assert_vlq(int32_t value, const char *expected) {
    char out[16];
    size_t len = 0;
    ASSERT(sourcemap_encode_vlq_to_buffer(value, out, sizeof(out), &len), "VLQ encode should succeed");
    ASSERT(len == strlen(expected), "VLQ length mismatch");
    ASSERT(memcmp(out, expected, len) == 0, "VLQ bytes mismatch");
}

TEST(vlq_known_values) {
    assert_vlq(0, "A");
    assert_vlq(1, "C");
    assert_vlq(-1, "D");
    assert_vlq(2, "E");
    assert_vlq(-2, "F");
    assert_vlq(16, "gB");
    assert_vlq(-16, "hB");
}

TEST(mappings_basic_lines) {
    SourceMapBuilder b;
    sourcemap_builder_init(&b);

    ASSERT(sourcemap_builder_add_mapping(&b, 0, 0, 0, 0, 0, -1), "first mapping should succeed");
    ASSERT(sourcemap_builder_add_mapping(&b, 1, 0, 0, 1, 0, -1), "second mapping should succeed");
    ASSERT(strcmp(sourcemap_builder_mappings(&b), "AAAA;AACA") == 0,
           "expected canonical two-line mapping");

    sourcemap_builder_free(&b);
}

TEST(mappings_with_name_and_sparse_lines) {
    SourceMapBuilder b;
    sourcemap_builder_init(&b);

    ASSERT(sourcemap_builder_add_mapping(&b, 2, 0, 0, 0, 0, 0), "sparse mapping should succeed");
    ASSERT(strcmp(sourcemap_builder_mappings(&b), ";;AAAAA") == 0,
           "expected sparse-line mapping with name field");

    sourcemap_builder_free(&b);
}

TEST(mappings_validation_nonmonotonic_column) {
    SourceMapBuilder b;
    sourcemap_builder_init(&b);

    ASSERT(sourcemap_builder_add_mapping(&b, 0, 3, 0, 0, 0, -1), "first mapping should succeed");
    ASSERT(!sourcemap_builder_add_mapping(&b, 0, 2, 0, 0, 0, -1),
           "non-monotonic generated column should fail");
    ASSERT(sourcemap_builder_mappings(&b) == NULL, "failed builder should report NULL mappings");

    sourcemap_builder_free(&b);
}

TEST(render_json_payload) {
    const char *sources[] = { "pages/index.mot", "components/Layout.mot" };
    const char *names[] = { "Layout" };
    char *json = sourcemap_render_json("index.wasm", sources, 2, names, 1, "AAAA;AACA");

    ASSERT(json != NULL, "render_json should return payload");
    ASSERT(contains(json, "\"version\":3"), "JSON should include version");
    ASSERT(contains(json, "\"file\":\"index.wasm\""), "JSON should include file");
    ASSERT(contains(json, "\"sources\":[\"pages/index.mot\",\"components/Layout.mot\"]"),
           "JSON should include sources");
    ASSERT(contains(json, "\"names\":[\"Layout\"]"), "JSON should include names");
    ASSERT(contains(json, "\"mappings\":\"AAAA;AACA\""), "JSON should include mappings");

    free(json);
}

int main(void) {
    printf("=== Source Map Tests ===\n");

    RUN_TEST(vlq_known_values);
    RUN_TEST(mappings_basic_lines);
    RUN_TEST(mappings_with_name_and_sparse_lines);
    RUN_TEST(mappings_validation_nonmonotonic_column);
    RUN_TEST(render_json_payload);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
