/*
 * Partial Evaluation tests
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "compiler/partial_eval.h"
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

/* Helper to parse an expression */
static AstNode *parse_expr(Arena *arena, const char *src) {
    Parser parser;
    char full_src[256];
    snprintf(full_src, sizeof(full_src), "<output %s>", src);
    parser_init(&parser, full_src, strlen(full_src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    if (!doc || !doc->data.document.children) return NULL;
    AstNode *output = doc->data.document.children;
    return output->data.output.expr;
}

TEST(literal_int) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "42");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_INT, "Should be int");
    ASSERT(val.v.integer == 42, "Should be 42");

    arena_destroy(arena);
}

TEST(literal_string) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "\"hello\"");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_STRING, "Should be string");
    ASSERT(strcmp(val.v.string.data, "hello") == 0, "Should be 'hello'");

    arena_destroy(arena);
}

TEST(literal_bool) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr_true = parse_expr(arena, "true");
    AstNode *expr_false = parse_expr(arena, "false");
    ASSERT(expr_true != NULL && expr_false != NULL, "Should parse expressions");

    PEValue val_true = pe_eval(pe, expr_true);
    PEValue val_false = pe_eval(pe, expr_false);

    ASSERT(val_true.kind == PE_BOOL, "true should be bool");
    ASSERT(val_true.v.boolean == true, "true should be true");
    ASSERT(val_false.kind == PE_BOOL, "false should be bool");
    ASSERT(val_false.v.boolean == false, "false should be false");

    arena_destroy(arena);
}

TEST(add_integers) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "1 + 2");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_INT, "Should be int");
    ASSERT(val.v.integer == 3, "1 + 2 = 3");

    arena_destroy(arena);
}

TEST(complex_arithmetic) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "2 * 3 + 4");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_INT || val.kind == PE_NUMBER, "Should be numeric");

    int64_t result;
    if (val.kind == PE_INT) {
        result = val.v.integer;
    } else {
        result = (int64_t)val.v.number;
    }
    ASSERT(result == 10, "2 * 3 + 4 = 10");

    arena_destroy(arena);
}

TEST(comparison) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    /* Test using direct PE functions (parser interprets <>/== as special chars) */
    PEValue val_lt = pe_lt(pe, pe_int(1), pe_int(2));
    PEValue val_gt = pe_gt(pe, pe_int(3), pe_int(2));
    PEValue val_eq = pe_eq(pe, pe_int(5), pe_int(5));
    PEValue val_neq = pe_neq(pe, pe_int(3), pe_int(4));
    PEValue val_lte = pe_lte(pe, pe_int(2), pe_int(2));
    PEValue val_gte = pe_gte(pe, pe_int(5), pe_int(3));

    ASSERT(val_lt.kind == PE_BOOL && val_lt.v.boolean == true, "1 < 2 is true");
    ASSERT(val_gt.kind == PE_BOOL && val_gt.v.boolean == true, "3 > 2 is true");
    ASSERT(val_eq.kind == PE_BOOL && val_eq.v.boolean == true, "5 == 5 is true");
    ASSERT(val_neq.kind == PE_BOOL && val_neq.v.boolean == true, "3 != 4 is true");
    ASSERT(val_lte.kind == PE_BOOL && val_lte.v.boolean == true, "2 <= 2 is true");
    ASSERT(val_gte.kind == PE_BOOL && val_gte.v.boolean == true, "5 >= 3 is true");

    /* Also test string comparisons */
    PEValue str_eq = pe_eq(pe, pe_string(arena, "abc", 3), pe_string(arena, "abc", 3));
    PEValue str_lt = pe_lt(pe, pe_string(arena, "abc", 3), pe_string(arena, "xyz", 3));
    ASSERT(str_eq.kind == PE_BOOL && str_eq.v.boolean == true, "\"abc\" == \"abc\" is true");
    ASSERT(str_lt.kind == PE_BOOL && str_lt.v.boolean == true, "\"abc\" < \"xyz\" is true");

    arena_destroy(arena);
}

TEST(logical_and) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "true and false");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_BOOL, "Should be bool");
    ASSERT(val.v.boolean == false, "true and false = false");

    arena_destroy(arena);
}

TEST(logical_or) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "false or true");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_BOOL, "Should be bool");
    ASSERT(val.v.boolean == true, "false or true = true");

    arena_destroy(arena);
}

TEST(string_concat) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "\"hello\" + \" world\"");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_STRING, "Should be string");
    ASSERT(strcmp(val.v.string.data, "hello world") == 0, "Should be 'hello world'");

    arena_destroy(arena);
}

TEST(uppercase_builtin) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    PEValue input = pe_string(arena, "hello", 5);
    PEValue result = pe_uppercase(pe, input);

    ASSERT(result.kind == PE_STRING, "Should be string");
    ASSERT(strcmp(result.v.string.data, "HELLO") == 0, "Should be 'HELLO'");

    arena_destroy(arena);
}

TEST(trim_builtin) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    PEValue input = pe_string(arena, "  hello  ", 9);
    PEValue result = pe_trim(pe, input);

    ASSERT(result.kind == PE_STRING, "Should be string");
    ASSERT(strcmp(result.v.string.data, "hello") == 0, "Should be 'hello'");

    arena_destroy(arena);
}

TEST(binding_static) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    /* Define a static binding */
    pe_define(pe, "x", pe_int(42), true);

    /* Look it up */
    PEBinding *binding = pe_lookup(pe, "x");
    ASSERT(binding != NULL, "Should find binding");
    ASSERT(binding->is_static, "Should be static");
    ASSERT(binding->value.kind == PE_INT, "Should be int");
    ASSERT(binding->value.v.integer == 42, "Should be 42");

    arena_destroy(arena);
}

TEST(scope_shadowing) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    /* Define x in outer scope */
    pe_define(pe, "x", pe_int(1), true);

    /* Push inner scope and shadow x */
    pe_push_scope(pe);
    pe_define(pe, "x", pe_int(2), true);

    /* Inner scope sees 2 */
    PEBinding *inner = pe_lookup(pe, "x");
    ASSERT(inner != NULL && inner->value.v.integer == 2, "Inner scope should see 2");

    /* Pop scope */
    pe_pop_scope(pe);

    /* Outer scope sees 1 */
    PEBinding *outer = pe_lookup(pe, "x");
    ASSERT(outer != NULL && outer->value.v.integer == 1, "Outer scope should see 1");

    arena_destroy(arena);
}

TEST(is_static) {
    Arena *arena = arena_create(4096);

    PEValue stat = pe_int(42);
    PEValue dyn = pe_dynamic();
    PEValue unk = pe_unknown();

    ASSERT(pe_is_static(&stat), "Int should be static");
    ASSERT(!pe_is_static(&dyn), "Dynamic should not be static");
    ASSERT(!pe_is_static(&unk), "Unknown should not be static");

    arena_destroy(arena);
}

TEST(array_literal) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "[1, 2, 3]");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_ARRAY, "Should be array");
    ASSERT(val.v.array.count == 3, "Should have 3 elements");
    ASSERT(val.v.array.elements[0].v.integer == 1, "First element should be 1");
    ASSERT(val.v.array.elements[1].v.integer == 2, "Second element should be 2");
    ASSERT(val.v.array.elements[2].v.integer == 3, "Third element should be 3");

    arena_destroy(arena);
}

TEST(negation) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "-42");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_INT, "Should be int");
    ASSERT(val.v.integer == -42, "Should be -42");

    arena_destroy(arena);
}

TEST(not_operator) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    AstNode *expr = parse_expr(arena, "not true");
    ASSERT(expr != NULL, "Should parse expression");

    PEValue val = pe_eval(pe, expr);
    ASSERT(val.kind == PE_BOOL, "Should be bool");
    ASSERT(val.v.boolean == false, "not true = false");

    arena_destroy(arena);
}

TEST(constants_folded_count) {
    Arena *arena = arena_create(4096);
    PartialEval *pe = pe_new(arena, NULL);

    /* Multiple folding operations */
    AstNode *expr = parse_expr(arena, "1 + 2 + 3 + 4");
    pe_eval(pe, expr);

    ASSERT(pe->constants_folded > 0, "Should have folded constants");
    ASSERT(pe->expressions_evaluated > 0, "Should have evaluated expressions");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Partial Evaluation Tests ===\n");

    RUN_TEST(literal_int);
    RUN_TEST(literal_string);
    RUN_TEST(literal_bool);
    RUN_TEST(add_integers);
    RUN_TEST(complex_arithmetic);
    RUN_TEST(comparison);
    RUN_TEST(logical_and);
    RUN_TEST(logical_or);
    RUN_TEST(string_concat);
    RUN_TEST(uppercase_builtin);
    RUN_TEST(trim_builtin);
    RUN_TEST(binding_static);
    RUN_TEST(scope_shadowing);
    RUN_TEST(is_static);
    RUN_TEST(array_literal);
    RUN_TEST(negation);
    RUN_TEST(not_operator);
    RUN_TEST(constants_folded_count);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
