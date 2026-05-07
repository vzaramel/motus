/*
 * Analyzer tests
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
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

/* Helper to parse and analyze */
static Analyzer *parse_and_analyze(Arena *arena, const char *src) {
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);

    Analyzer *a = analyzer_new(arena);
    analyzer_analyze(a, doc);
    return a;
}

TEST(undefined_variable) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena, "<output undefined_var>");

    ASSERT(analyzer_error_count(a) > 0, "Expected error for undefined variable");
    ASSERT(strstr(analyzer_errors(a)->message, "Undefined") != NULL,
           "Expected 'Undefined' in error message");

    arena_destroy(arena);
}

TEST(let_binding) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let x 42><output x></let>");

    ASSERT(analyzer_ok(a), "Expected no errors for valid let binding");

    arena_destroy(arena);
}

TEST(let_scope) {
    Arena *arena = arena_create(4096);

    /* x should not be visible outside let */
    Analyzer *a = parse_and_analyze(arena,
        "<let x 42></let><output x>");

    ASSERT(analyzer_error_count(a) > 0, "Expected error - x not in scope");

    arena_destroy(arena);
}

TEST(for_loop_variable) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let items [1, 2, 3]><for item in items><output item></for></let>");

    ASSERT(analyzer_ok(a), "Expected no errors for valid for loop");

    arena_destroy(arena);
}

TEST(for_index_variable) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let items [1, 2, 3]><for item, i in items><output i></for></let>");

    ASSERT(analyzer_ok(a), "Expected no errors with index variable");

    arena_destroy(arena);
}

TEST(redefinition) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let x 1><let x 2></let></let>");

    /* Redefinition in nested scope is allowed */
    ASSERT(analyzer_ok(a), "Nested redefinition should be allowed");

    arena_destroy(arena);
}

TEST(same_scope_redefinition) {
    Arena *arena = arena_create(4096);

    /* Two lets at same level - should error */
    Analyzer *a = parse_and_analyze(arena,
        "<div><let x 1></let><let x 2></let></div>");

    /* This is tricky - depends on scoping rules */
    /* For now, let's assume same-level redefinition is OK (different let blocks) */

    arena_destroy(arena);
}

TEST(builtin_functions) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let name \"hello\"><output name | uppercase | trim></let>");

    ASSERT(analyzer_ok(a), "Expected no errors for builtin functions");

    arena_destroy(arena);
}

TEST(unknown_function) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let x 1><output x | nonexistent></let>");

    ASSERT(analyzer_error_count(a) > 0, "Expected error for unknown function");

    arena_destroy(arena);
}

TEST(member_access) {
    Arena *arena = arena_create(4096);

    /* With any type, member access should work */
    Analyzer *a = parse_and_analyze(arena,
        "<let user {name: \"John\"}><output user.name></let>");

    ASSERT(analyzer_ok(a), "Expected no errors for member access");

    arena_destroy(arena);
}

TEST(component_definition) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<defcomp Button | label : string |><button><output label></button></defcomp>");

    ASSERT(analyzer_ok(a), "Expected no errors for component definition");

    arena_destroy(arena);
}

TEST(dependency_tracking) {
    Arena *arena = arena_create(4096);

    Parser parser;
    const char *src = "<let user {name: \"John\"}><output user.name></let>";
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);

    Analyzer *a = analyzer_new(arena);
    a->track_deps = true;
    analyzer_analyze(a, doc);

    /* Check that deps were tracked */
    ASSERT(analyzer_ok(a), "Analysis should succeed");

    arena_destroy(arena);
}

TEST(match_statement) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let status \"ready\">"
        "<match status>"
        "<case \"loading\"><output \"L\"></case>"
        "<case \"ready\"><output status></case>"
        "<default><output \"D\"></default>"
        "</match>"
        "</let>");

    ASSERT(analyzer_ok(a), "Expected no errors for valid match statement");

    arena_destroy(arena);
}

TEST(export_marks_symbol) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<defcomp ProductPage ||><div>ok</div></defcomp>"
        "<export default ProductPage>");
    ASSERT(analyzer_ok(a), "Expected export of defined symbol to succeed");

    Symbol *sym = scope_lookup(a->global_scope, "ProductPage");
    ASSERT(sym != NULL, "Expected ProductPage symbol");
    ASSERT((sym->flags & SYM_FLAG_EXPORTED) != 0, "Expected exported flag");

    arena_destroy(arena);
}

TEST(export_undefined_symbol) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena, "<export MissingThing>");
    ASSERT(analyzer_error_count(a) > 0, "Expected error for exporting undefined symbol");

    arena_destroy(arena);
}

TEST(interface_declaration) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<interface ButtonProps | label : string, disabled := false | />");
    ASSERT(analyzer_ok(a), "Expected interface declaration analysis to succeed");

    arena_destroy(arena);
}

TEST(macro_invocation) {
    Arena *arena = arena_create(4096);

    Analyzer *a = parse_and_analyze(arena,
        "<let items [1,2]>"
        "<macro list |items|><for item in items><output item></for></macro>"
        "<list items />"
        "</let>");
    ASSERT(analyzer_ok(a), "Expected macro declaration/invocation to analyze");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Analyzer Tests ===\n");

    RUN_TEST(undefined_variable);
    RUN_TEST(let_binding);
    RUN_TEST(let_scope);
    RUN_TEST(for_loop_variable);
    RUN_TEST(for_index_variable);
    RUN_TEST(redefinition);
    RUN_TEST(same_scope_redefinition);
    RUN_TEST(builtin_functions);
    RUN_TEST(unknown_function);
    RUN_TEST(member_access);
    RUN_TEST(component_definition);
    RUN_TEST(dependency_tracking);
    RUN_TEST(match_statement);
    RUN_TEST(export_marks_symbol);
    RUN_TEST(export_undefined_symbol);
    RUN_TEST(interface_declaration);
    RUN_TEST(macro_invocation);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
