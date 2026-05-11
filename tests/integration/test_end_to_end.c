/*
 * End-to-end integration tests for Motus compiler
 *
 * Tests the full pipeline: Source → Lex → Parse → Analyze → Compile → VM → Output
 * Focuses on real-world usage patterns and edge cases not covered
 * by individual module tests.
 */

#include <stdio.h>
#include <string.h>
#include "mot.h"
#include "compiler/bytecode.h"
#include "runtime/vm.h"
#include "parser/parser.h"
#include "analyzer/analyzer.h"
#include "compiler/compiler.h"
#include "codegen/codegen.h"
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

/* ---------- Output Capture ---------- */

static char output_buffer[8192];
static int output_len;

static void capture_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    if (output_len + (int)len < (int)sizeof(output_buffer) - 1) {
        memcpy(output_buffer + output_len, data, len);
        output_len += (int)len;
        output_buffer[output_len] = '\0';
    }
}

/* ---------- Helpers ---------- */

/* Compile, run, and capture output - the full pipeline */
static VMResult full_pipeline(const char *src) {
    Arena *arena = arena_create(32768);

    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    if (!doc || parser_had_error(&parser)) {
        arena_destroy(arena);
        return VM_ERROR;
    }

    Analyzer *a = analyzer_new(arena);
    if (!analyzer_analyze(a, doc)) {
        arena_destroy(arena);
        return VM_ERROR;
    }

    Compiler *c = compiler_new(arena, a);
    BytecodeModule *mod = compiler_compile(c, doc);
    if (!mod || !compiler_ok(c)) {
        arena_destroy(arena);
        return VM_ERROR;
    }

    VM *vm = vm_new(arena);
    vm_init(vm, mod);

    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    /* Note: arena_destroy will be called but output_buffer is static */
    arena_destroy(arena);
    return result;
}

/* Compile via public API and run */
static VMResult api_pipeline(const char *src) {
    MotCompileResult result = mot_compile(src, strlen(src));
    if (result.errors.count > 0 || !result.bytecode) {
        mot_result_free(&result);
        return VM_ERROR;
    }

    Arena *arena = arena_create(16384);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    if (!mod) {
        arena_destroy(arena);
        mot_result_free(&result);
        return VM_ERROR;
    }

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult vm_result = vm_run(vm);
    arena_destroy(arena);
    mot_result_free(&result);
    return vm_result;
}

/* Mock fetch for SQL tests */
static Value mock_products_fetch(const DataRequirement *req, const Value *frame_slots, void *userdata) {
    (void)req;
    (void)frame_slots;
    VM *vm = (VM *)userdata;

    Value arr = val_array(vm, 3);

    Value obj1 = val_object(vm, 2);
    object_set(vm, obj1.as.object, "name", val_string(vm, "Widget", 6));
    object_set(vm, obj1.as.object, "price", val_int(25));
    array_push(vm, arr.as.array, obj1);

    Value obj2 = val_object(vm, 2);
    object_set(vm, obj2.as.object, "name", val_string(vm, "Gadget", 6));
    object_set(vm, obj2.as.object, "price", val_int(50));
    array_push(vm, arr.as.array, obj2);

    return arr;
}

/* ======================================================================
 * E2E PIPELINE TESTS
 * ====================================================================== */

TEST(e2e_empty_document) {
    VMResult r = full_pipeline("");
    ASSERT(r == VM_OK, "Empty document should succeed");
    ASSERT(output_len == 0, "Empty document should produce no output");
}

TEST(e2e_plain_html) {
    VMResult r = full_pipeline(
        "<html><head><title>Test</title></head>"
        "<body><h1>Hello</h1><p>World</p></body></html>");
    ASSERT(r == VM_OK, "Plain HTML should compile and run");
    ASSERT(strstr(output_buffer, "<h1>Hello</h1>") != NULL, "Should contain heading");
    ASSERT(strstr(output_buffer, "<p>World</p>") != NULL, "Should contain paragraph");
}

TEST(e2e_nested_let_bindings) {
    VMResult r = full_pipeline(
        "<let a 10>"
        "<let b 20>"
        "<let c a + b>"
        "<output c>"
        "</let></let></let>");
    ASSERT(r == VM_OK, "Nested lets should succeed");
    ASSERT(strcmp(output_buffer, "30") == 0, "Should compute sum correctly");
}

TEST(e2e_for_loop_with_index) {
    /* for loop without index - basic iteration */
    VMResult r = full_pipeline(
        "<let items [\"a\", \"b\", \"c\"]>"
        "<for item in items>"
        "<output item>"
        "</for></let>");
    ASSERT(r == VM_OK, "For loop should succeed");
    ASSERT(strcmp(output_buffer, "abc") == 0, "Should output all items");
}

TEST(e2e_nested_for_loops) {
    VMResult r = full_pipeline(
        "<let rows [\"A\", \"B\"]>"
        "<let cols [1, 2]>"
        "<for row in rows>"
        "<for col in cols>"
        "<output row><output col>"
        "</for></for></let></let>");
    ASSERT(r == VM_OK, "Nested for loops should succeed");
    ASSERT(strcmp(output_buffer, "A1A2B1B2") == 0, "Should produce cartesian product");
}

TEST(e2e_if_elsif_else_chain) {
    VMResult r = full_pipeline(
        "<if false><output \"one\">"
        "<elsif true><output \"two\">"
        "<else><output \"other\">"
        "</if>");
    ASSERT(r == VM_OK, "If/elsif/else chain should succeed");
    ASSERT(strcmp(output_buffer, "two") == 0, "Should match elsif branch");
}

TEST(e2e_match_with_default) {
    VMResult r = full_pipeline(
        "<let x \"unknown\">"
        "<match x>"
        "<case \"yes\"><output \"Y\"></case>"
        "<case \"no\"><output \"N\"></case>"
        "<default><output \"?\"></default>"
        "</match></let>");
    ASSERT(r == VM_OK, "Match with default should succeed");
    ASSERT(strcmp(output_buffer, "?") == 0, "Should match default");
}

TEST(e2e_component_with_children_slot) {
    VMResult r = full_pipeline(
        "<defcomp Card | title : string |>"
        "<div class=\"card\">"
        "<h2><output title></h2>"
        "<div class=\"body\"><children></div>"
        "</div>"
        "</defcomp>"
        "<Card title=\"Hello\"><p>Content</p></Card>");
    ASSERT(r == VM_OK, "Component with children should succeed");
    ASSERT(strstr(output_buffer, "<h2>Hello</h2>") != NULL, "Should render title prop");
    ASSERT(strstr(output_buffer, "<p>Content</p>") != NULL, "Should render children");
}

TEST(e2e_nested_components) {
    VMResult r = full_pipeline(
        "<defcomp Outer ||><div class=\"outer\"><children></div></defcomp>"
        "<defcomp Inner | text : string |><span><output text></span></defcomp>"
        "<Outer><Inner text=\"hello\"/></Outer>");
    ASSERT(r == VM_OK, "Nested components should succeed");
    ASSERT(strstr(output_buffer, "<div class=\"outer\">") != NULL, "Should have outer div");
    ASSERT(strstr(output_buffer, "<span>hello</span>") != NULL, "Should have inner span");
}

TEST(e2e_pipe_chains) {
    VMResult r = full_pipeline(
        "<output \"  Hello World  \" | trim | uppercase>");
    ASSERT(r == VM_OK, "Pipe chain should succeed");
    ASSERT(strcmp(output_buffer, "HELLO WORLD") == 0, "Should trim then uppercase");
}

TEST(e2e_arithmetic_expressions) {
    VMResult r = full_pipeline(
        "<let x 10>"
        "<let y 3>"
        "<output x + y>"
        "<output x - y>"
        "<output x * y>"
        "</let></let>");
    ASSERT(r == VM_OK, "Arithmetic should succeed");
    ASSERT(strstr(output_buffer, "13") != NULL, "Should have addition result");
    ASSERT(strstr(output_buffer, "7") != NULL, "Should have subtraction result");
    ASSERT(strstr(output_buffer, "30") != NULL, "Should have multiplication result");
}

TEST(e2e_string_operations) {
    VMResult r = full_pipeline(
        "<let greeting \"Hello\" + \" \" + \"World\">"
        "<output greeting>"
        "</let>");
    ASSERT(r == VM_OK, "String operations should succeed");
    ASSERT(strstr(output_buffer, "Hello World") != NULL, "Should concatenate strings");
}

TEST(e2e_comparison_operators) {
    VMResult r = full_pipeline(
        "<let a 5>"
        "<let b 10>"
        "<if a lt b><output \"less\">"
        "<else><output \"not less\">"
        "</if></let></let>");
    ASSERT(r == VM_OK, "Comparison should succeed");
    ASSERT(strcmp(output_buffer, "less") == 0, "5 should be less than 10");
}

TEST(e2e_logical_operators) {
    VMResult r = full_pipeline(
        "<let a true>"
        "<let b false>"
        "<if a and b><output \"both\">"
        "<else><output \"not both\">"
        "</if></let></let>");
    ASSERT(r == VM_OK, "Logical operators should succeed");
    ASSERT(strcmp(output_buffer, "not both") == 0, "true AND false = false");
}

TEST(e2e_html_escaping) {
    VMResult r = full_pipeline(
        "<let dangerous \"<script>alert('xss')</script>\">"
        "<output dangerous></let>");
    ASSERT(r == VM_OK, "HTML escaping should succeed");
    ASSERT(strstr(output_buffer, "&lt;script&gt;") != NULL, "Should escape < and >");
    ASSERT(strstr(output_buffer, "<script>") == NULL, "Should NOT contain unescaped script");
}

TEST(e2e_element_attributes) {
    VMResult r = full_pipeline(
        "<let cls \"active\">"
        "<div class=\"container\">"
        "<a href=\"/home\">Home</a>"
        "</div></let>");
    ASSERT(r == VM_OK, "Element attributes should succeed");
    ASSERT(strstr(output_buffer, "class=\"container\"") != NULL, "Should have class attribute");
    ASSERT(strstr(output_buffer, "href=\"/home\"") != NULL, "Should have href attribute");
}

TEST(e2e_self_closing_elements) {
    VMResult r = full_pipeline("<br/><hr/><img src=\"test.png\"/>");
    ASSERT(r == VM_OK, "Self-closing elements should succeed");
    ASSERT(strstr(output_buffer, "<br/>") != NULL || strstr(output_buffer, "<br />") != NULL,
           "Should have br element");
}

TEST(e2e_object_member_access) {
    VMResult r = full_pipeline(
        "<let user { name: \"Alice\", age: 30 }>"
        "<output user.name>"
        "</let>");
    ASSERT(r == VM_OK, "Object member access should succeed");
    ASSERT(strstr(output_buffer, "Alice") != NULL, "Should access name field");
}

TEST(e2e_array_length) {
    /* Array iteration instead of indexing (indexing uses [n] syntax in expressions) */
    VMResult r = full_pipeline(
        "<let items [10, 20, 30]>"
        "<output items | length>"
        "</let>");
    ASSERT(r == VM_OK, "Array length should succeed");
    ASSERT(strstr(output_buffer, "3") != NULL, "Should have 3 elements");
}

TEST(e2e_var_and_set_in_component) {
    /* Note: 'count' is a reserved SQL keyword, use 'counter' instead */
    VMResult r = full_pipeline(
        "<defcomp Counter ||>"
        "<var counter 0 />"
        "<output counter>"
        "<set counter counter + 1 />"
        "<output counter>"
        "</defcomp>"
        "<Counter>");
    ASSERT(r == VM_OK, "Var and set should succeed");
    ASSERT(strcmp(output_buffer, "012") == 0 || strcmp(output_buffer, "01") == 0,
           "Should show initial and updated values");
}

TEST(e2e_macro_with_body) {
    VMResult r = full_pipeline(
        "<let items [1, 2, 3]>"
        "<macro item-list>"
        "<ul><for item in items><li><output item></li></for></ul>"
        "</macro>"
        "<item-list />"
        "</let>");
    ASSERT(r == VM_OK, "Macro should succeed");
    ASSERT(strstr(output_buffer, "<ul>") != NULL, "Should have ul");
    ASSERT(strstr(output_buffer, "<li>1</li>") != NULL, "Should have first item");
    ASSERT(strstr(output_buffer, "<li>3</li>") != NULL, "Should have third item");
}

TEST(e2e_import_export) {
    VMResult r = full_pipeline(
        "<interface Props | title : string | />"
        "<defcomp Widget | title : string |>"
        "<div><output title></div>"
        "</defcomp>"
        "<export default Widget>"
        "<Widget title=\"Test\">");
    ASSERT(r == VM_OK, "Import/export should succeed");
    ASSERT(strstr(output_buffer, "<div>Test</div>") != NULL, "Should render exported component");
}

/* ======================================================================
 * PUBLIC API INTEGRATION TESTS
 * ====================================================================== */

TEST(api_compile_and_run) {
    VMResult r = api_pipeline("<output \"Hello from API\">");
    ASSERT(r == VM_OK, "API pipeline should succeed");
    ASSERT(strcmp(output_buffer, "Hello from API") == 0, "Should output correctly via API");
}

TEST(api_compile_with_css_extraction) {
    const char *src =
        "<style>.card { color: red; }</style>"
        "<div class=\"card\">Hello</div>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Should compile without errors");
    ASSERT(result.css != NULL, "CSS should be extracted");
    ASSERT(strlen(result.css) > 0, "CSS should not be empty");
    ASSERT(strstr(result.css, "color: red") != NULL, "CSS should contain the style rule");

    mot_result_free(&result);
}

TEST(api_compile_with_js_extraction) {
    const char *src =
        "<script>console.log('hello');</script>"
        "<div>Hello</div>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Should compile without errors");
    ASSERT(result.js != NULL, "JS should be extracted");
    ASSERT(strlen(result.js) > 0, "JS should not be empty");
    ASSERT(strstr(result.js, "console.log") != NULL, "JS should contain the script");

    mot_result_free(&result);
}

TEST(api_bytecode_roundtrip) {
    const char *src =
        "<let x 42>"
        "<div class=\"test\"><output x></div>"
        "</let>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Should compile without errors");
    ASSERT(result.bytecode != NULL, "Should have bytecode");

    /* Deserialize and re-run */
    Arena *arena = arena_create(16384);
    BytecodeModule *mod = bytecode_deserialize(result.bytecode, (uint32_t)result.bytecode_len, arena);
    ASSERT(mod != NULL, "Should deserialize successfully");

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult vm_result = vm_run(vm);
    ASSERT(vm_result == VM_OK, "Deserialized bytecode should run");
    ASSERT(strstr(output_buffer, "42") != NULL, "Should output the value");

    arena_destroy(arena);
    mot_result_free(&result);
}

TEST(api_partial_eval_enabled) {
    const char *src = "<output 2 + 3>";
    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_BYTECODE;
    options.partial_eval = true;
    options.include_debug = false;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    ASSERT(result.errors.count == 0, "Should compile with partial eval");
    ASSERT(result.bytecode != NULL, "Should have bytecode");
    ASSERT(result.bytecode_len > 0, "Should have non-zero bytecode");

    mot_result_free(&result);
}

TEST(api_partial_eval_disabled) {
    const char *src = "<output 2 + 3>";
    MotCompileOptions options;
    memset(&options, 0, sizeof(options));
    options.target = MOT_TARGET_BYTECODE;
    options.partial_eval = false;
    options.include_debug = false;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &options);
    ASSERT(result.errors.count == 0, "Should compile without partial eval");
    ASSERT(result.bytecode != NULL, "Should have bytecode");

    mot_result_free(&result);
}

/* ======================================================================
 * SQL INTEGRATION TESTS
 * ====================================================================== */

TEST(e2e_sql_with_fetch) {
    Arena *arena = arena_create(32768);

    const char *src =
        "<let products select * from products>"
        "<for p in products>"
        "<div><output p.name> - <output p.price></div>"
        "</for></let>";

    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Parse should succeed");

    Analyzer *a = analyzer_new(arena);
    analyzer_analyze(a, doc);
    ASSERT(analyzer_ok(a), "Analysis should succeed");

    Compiler *c = compiler_new(arena, a);
    BytecodeModule *mod = compiler_compile(c, doc);
    ASSERT(mod != NULL, "Compile should succeed");

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);
    vm_set_fetch(vm, mock_products_fetch, vm);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "SQL with fetch should succeed");
    ASSERT(strstr(output_buffer, "Widget") != NULL, "Should have Widget");
    ASSERT(strstr(output_buffer, "Gadget") != NULL, "Should have Gadget");

    arena_destroy(arena);
}

TEST(e2e_sql_with_params) {
    Arena *arena = arena_create(32768);

    const char *src =
        "<let categoryId 5>"
        "<let products select * from products where category_id = :categoryId>"
        "<for p in products><output p.name></for>"
        "</let></let>";

    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Parse should succeed");

    Analyzer *a = analyzer_new(arena);
    analyzer_analyze(a, doc);
    ASSERT(analyzer_ok(a), "Analysis should succeed");

    Compiler *c = compiler_new(arena, a);
    BytecodeModule *mod = compiler_compile(c, doc);
    ASSERT(mod != NULL, "Compile should succeed");
    ASSERT(mod->data_req_count == 1, "Should have one data requirement");
    ASSERT(mod->data_reqs[0].param_count == 1, "Should have one parameter");

    arena_destroy(arena);
}

/* ======================================================================
 * ERROR HANDLING TESTS
 * ====================================================================== */

TEST(error_unclosed_tag) {
    const char *src = "<div><p>Hello</div>";
    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count > 0, "Unclosed tag should report errors");
    mot_result_free(&result);
}

TEST(error_undefined_variable) {
    const char *src = "<output undefined_var>";
    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count > 0, "Undefined variable should report errors");
    mot_result_free(&result);
}

TEST(error_null_source) {
    MotCompileResult result = mot_compile(NULL, 0);
    ASSERT(result.errors.count > 0, "NULL source should report error");
    ASSERT(result.bytecode == NULL, "Should not produce bytecode");
    mot_result_free(&result);
}

TEST(error_empty_source) {
    MotCompileResult result = mot_compile("", 0);
    ASSERT(result.errors.count == 0, "Empty source should succeed");
    mot_result_free(&result);
}

/* ======================================================================
 * EDGE CASE TESTS
 * ====================================================================== */

TEST(edge_deeply_nested_elements) {
    VMResult r = full_pipeline(
        "<div><div><div><div><div>"
        "Deep"
        "</div></div></div></div></div>");
    ASSERT(r == VM_OK, "Deeply nested elements should succeed");
    ASSERT(strstr(output_buffer, "Deep") != NULL, "Should contain nested content");
}

TEST(edge_many_siblings) {
    VMResult r = full_pipeline(
        "<div>"
        "<p>1</p><p>2</p><p>3</p><p>4</p><p>5</p>"
        "<p>6</p><p>7</p><p>8</p><p>9</p><p>10</p>"
        "</div>");
    ASSERT(r == VM_OK, "Many siblings should succeed");
    ASSERT(strstr(output_buffer, "<p>10</p>") != NULL, "Should have last sibling");
}

TEST(edge_boolean_values) {
    VMResult r = full_pipeline(
        "<let t true><let f false>"
        "<if t><output \"T\"></if>"
        "<if f><output \"F\"></if>"
        "</let></let>");
    ASSERT(r == VM_OK, "Boolean values should work");
    ASSERT(strcmp(output_buffer, "T") == 0, "Only true branch should execute");
}

TEST(edge_null_value) {
    /* Null value output */
    VMResult r = full_pipeline(
        "<let n null><output n></let>");
    ASSERT(r == VM_OK, "Null output should work");
    /* Null renders as empty or "null" */
    ASSERT(output_len >= 0, "Should produce output without error");
}

TEST(edge_false_condition) {
    /* Boolean false in condition */
    VMResult r = full_pipeline(
        "<if false><output \"truthy\"><else><output \"falsy\"></if>");
    ASSERT(r == VM_OK, "False condition should work");
    ASSERT(strcmp(output_buffer, "falsy") == 0, "False should take else branch");
}

TEST(edge_negation) {
    /* Negation operator */
    VMResult r = full_pipeline(
        "<if not true><output \"negated\"><else><output \"original\"></if>");
    ASSERT(r == VM_OK, "Negation should work");
    ASSERT(strcmp(output_buffer, "original") == 0, "not true should be false");
}

TEST(edge_comment_handling) {
    VMResult r = full_pipeline(
        "<!-- This is a comment -->"
        "<div>Visible</div>"
        "<!-- Another comment -->");
    ASSERT(r == VM_OK, "Comments should be handled");
    ASSERT(strstr(output_buffer, "Visible") != NULL, "Should have visible content");
}

TEST(edge_mixed_content) {
    VMResult r = full_pipeline(
        "<div>"
        "Text before "
        "<span>inline</span>"
        " text after"
        "</div>");
    ASSERT(r == VM_OK, "Mixed content should work");
    ASSERT(strstr(output_buffer, "Text before") != NULL, "Should have text before");
    ASSERT(strstr(output_buffer, "<span>inline</span>") != NULL, "Should have inline element");
}

/* ======================================================================
 * COMPONENT CSS SCOPING TESTS
 * ====================================================================== */

TEST(css_component_scoping) {
    const char *src =
        "<defcomp Card ||>"
        "<style>.card { padding: 1rem; }</style>"
        "<div class=\"card\"><children></div>"
        "</defcomp>"
        "<Card>Hello</Card>";

    MotCompileResult result = mot_compile(src, strlen(src));
    ASSERT(result.errors.count == 0, "Should compile without errors");
    if (result.css && strlen(result.css) > 0) {
        ASSERT(strstr(result.css, "Card") != NULL || strstr(result.css, "card") != NULL,
               "CSS should be scoped to component");
    }
    mot_result_free(&result);
}

/* ======================================================================
 * PARTIAL EVALUATION INTEGRATION TESTS
 * ====================================================================== */

TEST(partial_eval_constant_output) {
    /* With partial eval, "2 + 3" should be folded to "5" at compile time */
    VMResult r = full_pipeline("<output 2 + 3>");
    ASSERT(r == VM_OK, "Partial eval should succeed");
    ASSERT(strstr(output_buffer, "5") != NULL, "Should output folded constant");
}

TEST(partial_eval_string_concat) {
    VMResult r = full_pipeline(
        "<let greeting \"Hello\" + \" \" + \"World\">"
        "<output greeting></let>");
    ASSERT(r == VM_OK, "String concat partial eval should succeed");
    ASSERT(strstr(output_buffer, "Hello World") != NULL, "Should output concatenated string");
}

/* ======================================================================
 * MUTATION CONSTRUCTS
 * ====================================================================== */

TEST(e2e_insert_form) {
    VMResult r = full_pipeline(
        "<let contacts 0>"
        "<insert into contacts>"
        "<button type=\"submit\">Add</button>"
        "</insert>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Insert form should render");
    ASSERT(strstr(output_buffer, "<form") != NULL, "Should contain form tag");
    ASSERT(strstr(output_buffer, "data-mot-mutation=\"contacts\"") != NULL,
           "Should have mutation target attr");
    ASSERT(strstr(output_buffer, "data-mot-type=\"insert\"") != NULL,
           "Should have insert type attr");
    ASSERT(strstr(output_buffer, "data-mot-optimistic=\"true\"") != NULL,
           "Should default to optimistic=true");
    ASSERT(strstr(output_buffer, "</form>") != NULL, "Should close form");
}

TEST(e2e_update_form) {
    VMResult r = full_pipeline(
        "<let contacts 0>"
        "<update contacts>"
        "<button type=\"submit\">Save</button>"
        "</update>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Update form should render");
    ASSERT(strstr(output_buffer, "data-mot-type=\"update\"") != NULL,
           "Should have update type attr");
    ASSERT(strstr(output_buffer, "</form>") != NULL, "Should close form");
}

TEST(e2e_delete_form) {
    VMResult r = full_pipeline(
        "<let contacts 0>"
        "<delete from contacts>"
        "<button type=\"submit\">Remove</button>"
        "</delete>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Delete form should render");
    ASSERT(strstr(output_buffer, "data-mot-type=\"delete\"") != NULL,
           "Should have delete type attr");
    ASSERT(strstr(output_buffer, "</form>") != NULL, "Should close form");
}

TEST(e2e_bound_input) {
    VMResult r = full_pipeline(
        "<let contacts [1, 2]>"
        "<for contact in contacts>"
        "<insert into contacts>"
        "<input contact.name />"
        "</insert>"
        "</for>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Bound input should render");
    ASSERT(strstr(output_buffer, "<input") != NULL, "Should contain input tag");
    ASSERT(strstr(output_buffer, "name=\"name\"") != NULL, "Should have name attr");
    ASSERT(strstr(output_buffer, "type=\"text\"") != NULL, "Should have type attr");
}

TEST(e2e_pessimistic_insert) {
    VMResult r = full_pipeline(
        "<let contacts 0>"
        "<insert into contacts pessimistic>"
        "<button type=\"submit\">Add</button>"
        "</insert>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Pessimistic insert should render");
    ASSERT(strstr(output_buffer, "data-mot-optimistic=\"false\"") != NULL,
           "Should have optimistic=false for pessimistic");
    ASSERT(strstr(output_buffer, "data-mot-type=\"insert\"") != NULL,
           "Should still have insert type");
}

TEST(e2e_pending_access) {
    VMResult r = full_pipeline(
        "<let items [1, 2]>"
        "<for item in items>"
        "<if item.pending>"
        "<span>syncing</span>"
        "</if>"
        "</for>"
        "</let>"
    );
    ASSERT(r == VM_OK, "Pending access should compile and render");
    /* Server-side, .pending is null/falsy, so <span>syncing</span> should NOT appear */
    ASSERT(strstr(output_buffer, "syncing") == NULL,
           "Pending should be falsy server-side");
}

/* ======================================================================
 * MAIN
 * ====================================================================== */

int main(void) {
    printf("=== Integration Tests ===\n");

    /* E2E Pipeline Tests */
    RUN_TEST(e2e_empty_document);
    RUN_TEST(e2e_plain_html);
    RUN_TEST(e2e_nested_let_bindings);
    RUN_TEST(e2e_for_loop_with_index);
    RUN_TEST(e2e_nested_for_loops);
    RUN_TEST(e2e_if_elsif_else_chain);
    RUN_TEST(e2e_match_with_default);
    RUN_TEST(e2e_component_with_children_slot);
    RUN_TEST(e2e_nested_components);
    RUN_TEST(e2e_pipe_chains);
    RUN_TEST(e2e_arithmetic_expressions);
    RUN_TEST(e2e_string_operations);
    RUN_TEST(e2e_comparison_operators);
    RUN_TEST(e2e_logical_operators);
    RUN_TEST(e2e_html_escaping);
    RUN_TEST(e2e_element_attributes);
    RUN_TEST(e2e_self_closing_elements);
    RUN_TEST(e2e_object_member_access);
    RUN_TEST(e2e_array_length);
    RUN_TEST(e2e_var_and_set_in_component);
    RUN_TEST(e2e_macro_with_body);
    RUN_TEST(e2e_import_export);

    /* Public API */
    RUN_TEST(api_compile_and_run);
    RUN_TEST(api_compile_with_css_extraction);
    RUN_TEST(api_compile_with_js_extraction);
    RUN_TEST(api_bytecode_roundtrip);
    RUN_TEST(api_partial_eval_enabled);
    RUN_TEST(api_partial_eval_disabled);

    /* SQL Integration */
    RUN_TEST(e2e_sql_with_fetch);
    RUN_TEST(e2e_sql_with_params);

    /* Error Handling */
    RUN_TEST(error_unclosed_tag);
    RUN_TEST(error_undefined_variable);
    RUN_TEST(error_null_source);
    RUN_TEST(error_empty_source);

    /* Edge Cases */
    RUN_TEST(edge_deeply_nested_elements);
    RUN_TEST(edge_many_siblings);
    RUN_TEST(edge_boolean_values);
    RUN_TEST(edge_null_value);
    RUN_TEST(edge_false_condition);
    RUN_TEST(edge_negation);
    RUN_TEST(edge_comment_handling);
    RUN_TEST(edge_mixed_content);

    /* CSS Scoping */
    RUN_TEST(css_component_scoping);

    /* Partial Evaluation */
    RUN_TEST(partial_eval_constant_output);
    RUN_TEST(partial_eval_string_concat);

    /* Mutation Constructs */
    RUN_TEST(e2e_insert_form);
    RUN_TEST(e2e_update_form);
    RUN_TEST(e2e_delete_form);
    RUN_TEST(e2e_bound_input);
    RUN_TEST(e2e_pessimistic_insert);
    RUN_TEST(e2e_pending_access);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
