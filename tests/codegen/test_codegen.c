/*
 * Codegen tests - CSS/JS extraction and scoping
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "parser/ast.h"
#include "codegen/codegen.h"
#include "util/arena.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s...", #name); \
    fflush(stdout); \
    tests_run++; \
    test_##name(); \
    tests_passed++; \
    printf(" PASSED\n"); \
    fflush(stdout); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf(" FAILED: %s\n", msg); \
        return; \
    } \
} while(0)

/* Parse source and return AST */
static AstNode *parse(Arena *arena, const char *src) {
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    return parser_parse(&parser);
}

TEST(global_css_extraction) {
    Arena *arena = arena_create(8192);
    const char *src = "<style>.button { color: red; }</style>";

    AstNode *doc = parse(arena, src);
    ASSERT(doc != NULL, "Expected document");

    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result != NULL, "Expected result");
    ASSERT(result->css_count == 1, "Expected 1 CSS block");
    ASSERT(result->css != NULL, "Expected CSS content");

    /* Global CSS should not be scoped */
    ASSERT(strstr(result->css->content, ".button") != NULL, "Expected .button selector");
    ASSERT(strstr(result->css->content, "color: red") != NULL, "Expected color property");

    arena_destroy(arena);
}

TEST(global_js_extraction) {
    Arena *arena = arena_create(8192);
    const char *src = "<script>console.log('hello');</script>";

    AstNode *doc = parse(arena, src);
    ASSERT(doc != NULL, "Expected document");

    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result != NULL, "Expected result");
    ASSERT(result->js_count == 1, "Expected 1 JS block");
    ASSERT(result->js != NULL, "Expected JS content");

    ASSERT(strstr(result->js->content, "console.log") != NULL, "Expected console.log");

    arena_destroy(arena);
}

TEST(component_css_scoping) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<defcomp Button ||>"
        "<style>.btn { padding: 10px; }</style>"
        "<button class=\"btn\"><children></button>"
        "</defcomp>";

    AstNode *doc = parse(arena, src);
    ASSERT(doc != NULL, "Expected document");

    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result != NULL, "Expected result");
    ASSERT(result->css_count == 1, "Expected 1 CSS block");

    /* CSS inside component should be scoped */
    ASSERT(result->css->component != NULL, "Expected component name");
    ASSERT(strcmp(result->css->component, "Button") == 0, "Expected component 'Button'");
    ASSERT(strstr(result->css->content, "[data-component=\"Button\"]") != NULL,
           "Expected scoped selector");

    arena_destroy(arena);
}

TEST(multiple_css_blocks) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<style>.global { margin: 0; }</style>"
        "<defcomp Card ||>"
        "<style>.card { border: 1px solid; }</style>"
        "<div class=\"card\"><children></div>"
        "</defcomp>";

    AstNode *doc = parse(arena, src);
    ASSERT(doc != NULL, "Expected document");

    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result != NULL, "Expected result");
    ASSERT(result->css_count == 2, "Expected 2 CSS blocks");

    /* First block should be global (unscoped) */
    ASSERT(result->css->component == NULL, "First should be global");

    /* Second block should be scoped to Card */
    ASSERT(result->css->next != NULL, "Expected second block");
    ASSERT(result->css->next->component != NULL, "Second should be scoped");
    ASSERT(strcmp(result->css->next->component, "Card") == 0, "Expected 'Card'");

    arena_destroy(arena);
}

TEST(css_combine) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<style>body { margin: 0; }</style>"
        "<style>h1 { font-size: 2em; }</style>";

    AstNode *doc = parse(arena, src);
    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result->css_count == 2, "Expected 2 CSS blocks");

    char *combined = codegen_combine_css(arena, result);
    ASSERT(combined != NULL, "Expected combined CSS");
    ASSERT(strstr(combined, "body") != NULL, "Expected body selector");
    ASSERT(strstr(combined, "h1") != NULL, "Expected h1 selector");

    arena_destroy(arena);
}

TEST(js_combine) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<script>var a = 1;</script>"
        "<script>var b = 2;</script>";

    AstNode *doc = parse(arena, src);
    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result->js_count == 2, "Expected 2 JS blocks");

    char *combined = codegen_combine_js(arena, result);
    ASSERT(combined != NULL, "Expected combined JS");
    ASSERT(strstr(combined, "var a = 1") != NULL, "Expected first var");
    ASSERT(strstr(combined, "var b = 2") != NULL, "Expected second var");

    arena_destroy(arena);
}

TEST(css_scope_function) {
    Arena *arena = arena_create(8192);

    const char *css = ".btn { color: blue; }";
    char *scoped = css_scope(arena, css, strlen(css), "Button");

    ASSERT(strstr(scoped, "[data-component=\"Button\"]") != NULL,
           "Expected component attribute selector");
    ASSERT(strstr(scoped, ".btn") != NULL, "Expected .btn class");

    arena_destroy(arena);
}

TEST(css_scope_multiple_selectors) {
    Arena *arena = arena_create(8192);

    const char *css = ".btn, .button { padding: 5px; }";
    char *scoped = css_scope(arena, css, strlen(css), "Test");

    /* Both selectors should be scoped */
    char *first = strstr(scoped, "[data-component=\"Test\"] .btn");
    char *second = strstr(scoped, "[data-component=\"Test\"] .button");
    ASSERT(first != NULL, "Expected scoped .btn");
    ASSERT(second != NULL, "Expected scoped .button");

    arena_destroy(arena);
}

TEST(nested_component_css) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<defcomp Outer ||>"
        "<style>.outer { display: flex; }</style>"
        "<div class=\"outer\">"
        "<defcomp Inner ||>"
        "<style>.inner { padding: 10px; }</style>"
        "<span class=\"inner\"><children></span>"
        "</defcomp>"
        "</div>"
        "</defcomp>";

    AstNode *doc = parse(arena, src);
    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result->css_count == 2, "Expected 2 CSS blocks");

    /* First should be Outer */
    ASSERT(strcmp(result->css->component, "Outer") == 0, "First should be Outer");
    ASSERT(strstr(result->css->content, "[data-component=\"Outer\"]") != NULL,
           "Expected Outer scope");

    /* Second should be Inner */
    ASSERT(strcmp(result->css->next->component, "Inner") == 0, "Second should be Inner");
    ASSERT(strstr(result->css->next->content, "[data-component=\"Inner\"]") != NULL,
           "Expected Inner scope");

    arena_destroy(arena);
}

TEST(empty_result) {
    Arena *arena = arena_create(8192);
    const char *src = "<div>No styles or scripts</div>";

    AstNode *doc = parse(arena, src);
    CodegenResult *result = codegen_extract(arena, doc);
    ASSERT(result != NULL, "Expected result");
    ASSERT(result->css_count == 0, "Expected no CSS");
    ASSERT(result->js_count == 0, "Expected no JS");

    char *css = codegen_combine_css(arena, result);
    ASSERT(css != NULL && css[0] == '\0', "Expected empty CSS string");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Codegen Tests ===\n");

    RUN_TEST(global_css_extraction);
    RUN_TEST(global_js_extraction);
    RUN_TEST(component_css_scoping);
    RUN_TEST(multiple_css_blocks);
    RUN_TEST(css_combine);
    RUN_TEST(js_combine);
    RUN_TEST(css_scope_function);
    RUN_TEST(css_scope_multiple_selectors);
    RUN_TEST(nested_component_css);
    RUN_TEST(empty_result);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
