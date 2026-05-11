/*
 * Bytecode debug metadata tests
 */

#include <stdio.h>
#include <string.h>

#include "mot.h"
#include "compiler/bytecode.h"

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

static int skip_string(const uint8_t *data, size_t len, size_t *pos) {
    uint32_t n = 0;
    if (!read_u32_at(data, len, pos, &n)) return 0;
    return skip_bytes(len, pos, n);
}

static int parse_debug_trailer(const uint8_t *data, size_t len,
                               uint32_t *out_span_count,
                               uint32_t *out_query_count,
                               uint32_t *out_first_query_line) {
    size_t pos = 0;
    uint32_t const_count = 0, string_count = 0, data_req_count = 0;
    uint32_t dep_count = 0, builtin_count = 0, comp_ref_count = 0;
    uint32_t mut_req_count = 0, func_count = 0;
    uint32_t tmp_u32 = 0;
    uint16_t tmp_u16 = 0;
    uint8_t tmp_u8 = 0;

    if (!read_u32_at(data, len, &pos, &tmp_u32)) return 0; /* magic */
    if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* major */
    if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* minor */
    if (!skip_bytes(len, &pos, 2)) return 0; /* flags */

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
            if (!skip_string(data, len, &pos)) return 0;
        } else {
            return 0;
        }
    }

    for (uint32_t i = 0; i < string_count; i++) {
        if (!skip_string(data, len, &pos)) return 0;
    }

    for (uint32_t i = 0; i < data_req_count; i++) {
        if (!skip_string(data, len, &pos)) return 0;
        if (!skip_bytes(len, &pos, 1 + 1 + 4 + 4)) return 0;
        if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0;
        for (uint16_t p = 0; p < tmp_u16; p++) {
            if (!skip_string(data, len, &pos)) return 0;
            if (!skip_bytes(len, &pos, 2)) return 0;
        }
    }

    for (uint32_t i = 0; i < dep_count; i++) {
        if (!skip_string(data, len, &pos)) return 0;
    }

    for (uint32_t i = 0; i < builtin_count; i++) {
        if (!skip_string(data, len, &pos)) return 0;
        if (!skip_bytes(len, &pos, 2)) return 0;
    }

    for (uint32_t i = 0; i < comp_ref_count; i++) {
        if (!skip_string(data, len, &pos) || !skip_string(data, len, &pos)) return 0;
    }

    for (uint32_t i = 0; i < mut_req_count; i++) {
        if (!skip_bytes(len, &pos, 1)) return 0; /* type */
        if (!skip_string(data, len, &pos)) return 0; /* target */
        if (!read_u16_at(data, len, &pos, &tmp_u16)) return 0; /* field_count */
        for (uint16_t f = 0; f < tmp_u16; f++) {
            if (!skip_string(data, len, &pos)) return 0;
        }
    }

    if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    for (uint32_t i = 0; i < func_count; i++) {
        if (!read_u32_at(data, len, &pos, &tmp_u32) || !skip_bytes(len, &pos, tmp_u32)) return 0;
    }

    if (!read_u32_at(data, len, &pos, &tmp_u32)) return 0;
    if (tmp_u32 != BYTECODE_DEBUG_MAGIC) return 0;

    if (!read_u32_at(data, len, &pos, out_span_count)) return 0;
    for (uint32_t i = 0; i < *out_span_count; i++) {
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

TEST(debug_trailer_present_for_simple_page) {
    const char *src = "<div><output \"ok\"></div>";
    MotCompileResult result = mot_compile(src, strlen(src));
    uint32_t span_count = 0;
    uint32_t query_count = 0;
    uint32_t first_query_line = 0;

    ASSERT(result.errors.count == 0, "compile should succeed");
    ASSERT(result.bytecode != NULL, "bytecode should exist");
    ASSERT(result.bytecode_len > 0, "bytecode should not be empty");
    ASSERT(parse_debug_trailer(result.bytecode, result.bytecode_len, &span_count, &query_count, &first_query_line),
           "debug trailer should parse");
    ASSERT(span_count > 0, "span count should be present");
    ASSERT(query_count == 0, "simple page should have no query entries");

    mot_result_free(&result);
}

TEST(debug_query_metadata_carries_source_line) {
    const char *src =
        "<let productId 42>"
        "<let product dynamic single select id from products where id = :productId>"
        "<output productId>"
        "</let>"
        "</let>";

    MotCompileResult result = mot_compile(src, strlen(src));
    uint32_t span_count = 0;
    uint32_t query_count = 0;
    uint32_t first_query_line = 0;

    ASSERT(result.errors.count == 0, "compile should succeed");
    ASSERT(parse_debug_trailer(result.bytecode, result.bytecode_len, &span_count, &query_count, &first_query_line),
           "debug trailer should parse");
    ASSERT(span_count > 0, "query page should contain spans");
    ASSERT(query_count == 1, "query page should emit one query metadata entry");
    ASSERT(first_query_line > 0, "query metadata should include source line");

    mot_result_free(&result);
}

int main(void) {
    printf("=== Debug Metadata Tests ===\n");

    RUN_TEST(debug_trailer_present_for_simple_page);
    RUN_TEST(debug_query_metadata_carries_source_line);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
