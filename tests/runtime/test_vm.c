/*
 * VM tests
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "analyzer/analyzer.h"
#include "compiler/compiler.h"
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

/* Output capture buffer */
static char output_buffer[4096];
static int output_len;

static void capture_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    if (output_len + len < sizeof(output_buffer) - 1) {
        memcpy(output_buffer + output_len, data, len);
        output_len += len;
        output_buffer[output_len] = '\0';
    }
}

/* Helper to compile and run source */
static VMResult run_source(Arena *arena, const char *src) {
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    if (!doc) return VM_ERROR;

    Analyzer *a = analyzer_new(arena);
    analyzer_analyze(a, doc);
    if (!analyzer_ok(a)) return VM_ERROR;

    Compiler *c = compiler_new(arena, a);
    BytecodeModule *mod = compiler_compile(c, doc);
    if (!mod) return VM_ERROR;

    VM *vm = vm_new(arena);
    vm_init(vm, mod);

    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    return vm_run(vm);
}

TEST(simple_output) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<output \"Hello\">");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "Hello") == 0, "Should output 'Hello'");

    arena_destroy(arena);
}

TEST(html_element) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<div>Hello</div>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "<div>Hello</div>") == 0, "Should output HTML element");

    arena_destroy(arena);
}

TEST(html_escape) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<output \"<script>\">");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "&lt;script&gt;") == 0, "Should escape HTML");

    arena_destroy(arena);
}

TEST(integer_arithmetic) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<let x 1 + 2><output x></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "3") == 0, "Should output 3");

    arena_destroy(arena);
}

TEST(complex_arithmetic) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<let x 2 * 3 + 4><output x></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "10") == 0, "Should output 10");

    arena_destroy(arena);
}

TEST(string_concat) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<let x \"Hello\" + \" \" + \"World\"><output x></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "Hello World") == 0, "Should concatenate strings");

    arena_destroy(arena);
}

TEST(if_true) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let x true><if x><output \"yes\"><else><output \"no\"></if></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "yes") == 0, "Should output 'yes'");

    arena_destroy(arena);
}

TEST(if_false) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let x false><if x><output \"yes\"><else><output \"no\"></if></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "no") == 0, "Should output 'no'");

    arena_destroy(arena);
}

TEST(ternary_expression) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let flag true><output if flag then \"on\" else \"off\"></let>");
    ASSERT(result == VM_OK, "Should succeed");
    /* Reactive binding system wraps variable-referencing outputs in spans */
    ASSERT(strstr(output_buffer, "on") != NULL, "Should output ternary true branch");

    arena_destroy(arena);
}

TEST(match_statement) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let status \"ready\">"
        "<match status>"
        "<case \"loading\"><output \"L\"></case>"
        "<case \"ready\"><output \"R\"></case>"
        "<default><output \"D\"></default>"
        "</match>"
        "</let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "R") == 0, "Should output matching case");

    arena_destroy(arena);
}

TEST(interface_and_export) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<interface CardProps | title : string | />"
        "<defcomp Card | title : string |><div><output title></div></defcomp>"
        "<export default Card>"
        "<Card title=\"Items\">");
    ASSERT(result == VM_OK, "Should succeed with interface/export metadata");
    ASSERT(strcmp(output_buffer, "<div>Items</div>") == 0, "Should render component output");

    arena_destroy(arena);
}

TEST(macro_expansion) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let cart { items: [1, 2, 3] }>"
        "<macro cart-count><span><for item in cart.items><output item></for></span></macro>"
        "<cart-count />"
        "</let>");
    ASSERT(result == VM_OK, "Should succeed for macro expansion");
    if (strcmp(output_buffer, "<span>123</span>") != 0) {
        printf(" FAILED: macro output mismatch, got '%s'\n", output_buffer);
        test_failed = 1;
        arena_destroy(arena);
        return;
    }

    arena_destroy(arena);
}

TEST(for_loop) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let items [1, 2, 3]><for item in items><output item></for></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "123") == 0, "Should output '123'");

    arena_destroy(arena);
}

TEST(for_loop_html) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let items [\"a\", \"b\"]><ul><for item in items><li><output item></li></for></ul></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "<ul><li>a</li><li>b</li></ul>") == 0,
           "Should output HTML list");

    arena_destroy(arena);
}

TEST(builtin_uppercase) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<output \"hello\" | uppercase>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "HELLO") == 0, "Should uppercase");

    arena_destroy(arena);
}

TEST(builtin_trim) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<output \"  hello  \" | trim>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "hello") == 0, "Should trim");

    arena_destroy(arena);
}

TEST(pipe_chain) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena, "<output \"  hello  \" | trim | uppercase>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "HELLO") == 0, "Should trim then uppercase");

    arena_destroy(arena);
}

TEST(elsif_single_branch) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<if false><output \"A\"><elsif true><output \"B\"><else><output \"C\"></if>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "B") == 0, "Should output elsif branch");

    arena_destroy(arena);
}

TEST(elsif_chain_late_match) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<if false><output \"A\"><elsif false><output \"B\"><elsif true><output \"C\"><else><output \"D\"></if>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "C") == 0, "Should output second elsif branch");

    arena_destroy(arena);
}

TEST(builtin_many_args) {
    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_module_new(arena);

    uint16_t uppercase_idx = bytecode_add_builtin(mod, "uppercase", 1, 1);

    for (int i = 0; i < 9; i++) {
        const char *lit = i == 0 ? "a" : "x";
        Constant c = {0};
        c.type = CONST_STRING;
        c.v.string.data = arena_strdup(arena, lit);
        c.v.string.length = 1;
        uint16_t const_idx = bytecode_add_constant(mod, c);
        chunk_write(&mod->main, BC_CONST, 1, arena);
        chunk_write_u16(&mod->main, const_idx, 1, arena);
    }

    chunk_write(&mod->main, BC_CALL_BUILTIN, 1, arena);
    chunk_write_u16(&mod->main, uppercase_idx, 1, arena);
    chunk_write(&mod->main, 9, 1, arena);
    chunk_write(&mod->main, BC_EMIT_TEXT, 1, arena);
    chunk_write(&mod->main, BC_HALT, 1, arena);

    VM *vm = vm_new(arena);
    vm_init(vm, mod);

    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Should handle builtin calls with many args safely");
    ASSERT(strcmp(output_buffer, "A") == 0, "Should still execute builtin correctly");

    arena_destroy(arena);
}

TEST(opcode_and_or) {
    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_module_new(arena);

    chunk_write(&mod->main, BC_TRUE, 1, arena);
    chunk_write(&mod->main, BC_FALSE, 1, arena);
    chunk_write(&mod->main, BC_AND, 1, arena);
    chunk_write(&mod->main, BC_EMIT_TEXT, 1, arena);

    chunk_write(&mod->main, BC_TRUE, 1, arena);
    chunk_write(&mod->main, BC_FALSE, 1, arena);
    chunk_write(&mod->main, BC_OR, 1, arena);
    chunk_write(&mod->main, BC_EMIT_TEXT, 1, arena);

    chunk_write(&mod->main, BC_HALT, 1, arena);

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Should execute BC_AND and BC_OR");
    ASSERT(strcmp(output_buffer, "falsetrue") == 0, "Should emit logical results");

    arena_destroy(arena);
}

TEST(opcode_concat) {
    Arena *arena = arena_create(8192);
    BytecodeModule *mod = bytecode_module_new(arena);

    const char *parts[] = {"A", "B", "C"};
    for (int i = 0; i < 3; i++) {
        Constant c = {0};
        c.type = CONST_STRING;
        c.v.string.data = arena_strdup(arena, parts[i]);
        c.v.string.length = 1;
        uint16_t idx = bytecode_add_constant(mod, c);
        chunk_write(&mod->main, BC_CONST, 1, arena);
        chunk_write_u16(&mod->main, idx, 1, arena);
    }

    chunk_write(&mod->main, BC_CONCAT, 1, arena);
    chunk_write(&mod->main, 3, 1, arena);
    chunk_write(&mod->main, BC_EMIT_TEXT, 1, arena);
    chunk_write(&mod->main, BC_HALT, 1, arena);

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Should execute BC_CONCAT");
    ASSERT(strcmp(output_buffer, "ABC") == 0, "Should concatenate strings");

    arena_destroy(arena);
}

TEST(opcode_call_and_call_pipe) {
    Arena *arena = arena_create(16384);
    BytecodeModule *mod = bytecode_module_new(arena);

    uint16_t fn_emit_f = bytecode_add_function(mod);
    Chunk *fn_chunk = bytecode_get_function(mod, fn_emit_f);

    Constant f = {0};
    f.type = CONST_STRING;
    f.v.string.data = arena_strdup(arena, "F");
    f.v.string.length = 1;
    uint16_t f_idx = bytecode_add_constant(mod, f);
    chunk_write(fn_chunk, BC_CONST, 1, arena);
    chunk_write_u16(fn_chunk, f_idx, 1, arena);
    chunk_write(fn_chunk, BC_EMIT_TEXT, 1, arena);
    chunk_write(fn_chunk, BC_RETURN, 1, arena);

    uint16_t fn_emit_arg = bytecode_add_function(mod);
    Chunk *pipe_chunk = bytecode_get_function(mod, fn_emit_arg);
    chunk_write(pipe_chunk, BC_LOAD, 1, arena);
    chunk_write_u16(pipe_chunk, 0, 1, arena);
    chunk_write(pipe_chunk, BC_EMIT_TEXT, 1, arena);
    chunk_write(pipe_chunk, BC_RETURN, 1, arena);

    Constant p = {0};
    p.type = CONST_STRING;
    p.v.string.data = arena_strdup(arena, "P");
    p.v.string.length = 1;
    uint16_t p_idx = bytecode_add_constant(mod, p);

    chunk_write(&mod->main, BC_CALL, 1, arena);
    chunk_write_u16(&mod->main, fn_emit_f, 1, arena);
    chunk_write(&mod->main, 0, 1, arena);

    chunk_write(&mod->main, BC_CONST, 1, arena);
    chunk_write_u16(&mod->main, p_idx, 1, arena);
    chunk_write(&mod->main, BC_CALL_PIPE, 1, arena);
    chunk_write_u16(&mod->main, fn_emit_arg, 1, arena);

    chunk_write(&mod->main, BC_HALT, 1, arena);

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Should execute BC_CALL and BC_CALL_PIPE");
    ASSERT(strcmp(output_buffer, "FP") == 0, "Should output from call and pipe call");

    arena_destroy(arena);
}

TEST(component_stack_cleanup) {
    Arena *arena = arena_create(32768);
    BytecodeModule *mod = bytecode_module_new(arena);

    uint16_t comp_idx = bytecode_add_function(mod);
    Chunk *comp_chunk = bytecode_get_function(mod, comp_idx);
    chunk_write(comp_chunk, BC_RETURN, 1, arena);

    /* Stress repeated component calls; stack must not leak across returns. */
    for (int i = 0; i < 180; i++) {
        chunk_write(&mod->main, BC_OBJECT_NEW, 1, arena);
        chunk_write_u16(&mod->main, 0, 1, arena);
        chunk_write(&mod->main, BC_NULL, 1, arena);
        chunk_write(&mod->main, BC_COMPONENT_START, 1, arena);
        chunk_write_u16(&mod->main, comp_idx, 1, arena);
    }
    chunk_write(&mod->main, BC_HALT, 1, arena);

    VM *vm = vm_new(arena);
    vm_init(vm, mod);
    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Repeated component calls should not overflow stack");

    arena_destroy(arena);
}

TEST(vm_step_basic) {
    Arena *arena = arena_create(8192);

    Parser parser;
    parser_init(&parser, "<output \"Hi\">", strlen("<output \"Hi\">"), arena, NULL);
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

    ASSERT(vm_step(vm) == VM_OK, "Step 1 should succeed");
    ASSERT(vm_step(vm) == VM_OK, "Step 2 should succeed");
    ASSERT(strcmp(output_buffer, "Hi") == 0, "Second step should emit output");
    ASSERT(vm_step(vm) == VM_OK, "Step 3 should halt successfully");

    arena_destroy(arena);
}

TEST(nested_let) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let x 1><let y 2><let z x + y><output z></let></let></let>");
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "3") == 0, "Should output 3");

    arena_destroy(arena);
}

TEST(value_constructors) {
    Arena *arena = arena_create(8192);
    VM *vm = vm_new(arena);

    Value null_v = val_null();
    ASSERT(null_v.type == VAL_NULL, "Should be null");

    Value bool_v = val_bool(true);
    ASSERT(bool_v.type == VAL_BOOL && bool_v.as.boolean == true, "Should be true");

    Value int_v = val_int(42);
    ASSERT(int_v.type == VAL_INT && int_v.as.integer == 42, "Should be 42");

    Value num_v = val_number(3.14);
    ASSERT(num_v.type == VAL_NUMBER && num_v.as.number == 3.14, "Should be 3.14");

    Value str_v = val_string(vm, "hello", 5);
    ASSERT(str_v.type == VAL_STRING &&
           strcmp(str_v.as.string.data, "hello") == 0, "Should be 'hello'");

    arena_destroy(arena);
}

TEST(value_is_truthy) {
    Arena *arena = arena_create(8192);
    VM *vm = vm_new(arena);

    Value null_v = val_null();
    ASSERT(!val_is_truthy(&null_v), "null should be falsy");

    Value false_v = val_bool(false);
    ASSERT(!val_is_truthy(&false_v), "false should be falsy");

    Value true_v = val_bool(true);
    ASSERT(val_is_truthy(&true_v), "true should be truthy");

    Value zero = val_int(0);
    ASSERT(!val_is_truthy(&zero), "0 should be falsy");

    Value one = val_int(1);
    ASSERT(val_is_truthy(&one), "1 should be truthy");

    Value empty_str = val_string(vm, "", 0);
    ASSERT(!val_is_truthy(&empty_str), "empty string should be falsy");

    Value str = val_string(vm, "hi", 2);
    ASSERT(val_is_truthy(&str), "non-empty string should be truthy");

    arena_destroy(arena);
}

TEST(value_equals) {
    Arena *arena = arena_create(8192);
    VM *vm = vm_new(arena);

    Value a = val_int(42);
    Value b = val_int(42);
    ASSERT(val_equals(&a, &b), "42 == 42");

    Value c = val_int(1);
    ASSERT(!val_equals(&a, &c), "42 != 1");

    Value s1 = val_string(vm, "hello", 5);
    Value s2 = val_string(vm, "hello", 5);
    ASSERT(val_equals(&s1, &s2), "strings should be equal");

    Value s3 = val_string(vm, "world", 5);
    ASSERT(!val_equals(&s1, &s3), "different strings should not be equal");

    arena_destroy(arena);
}

TEST(array_operations) {
    Arena *arena = arena_create(8192);
    VM *vm = vm_new(arena);

    Value arr = val_array(vm, 4);
    array_push(vm, arr.as.array, val_int(1));
    array_push(vm, arr.as.array, val_int(2));
    array_push(vm, arr.as.array, val_int(3));

    ASSERT(arr.as.array->count == 3, "Should have 3 elements");
    ASSERT(array_get(arr.as.array, 0).as.integer == 1, "First element should be 1");
    ASSERT(array_get(arr.as.array, 1).as.integer == 2, "Second element should be 2");
    ASSERT(array_get(arr.as.array, 2).as.integer == 3, "Third element should be 3");

    arena_destroy(arena);
}

TEST(object_operations) {
    Arena *arena = arena_create(8192);
    VM *vm = vm_new(arena);

    Value obj = val_object(vm, 4);
    object_set(vm, obj.as.object, "name", val_string(vm, "John", 4));
    object_set(vm, obj.as.object, "age", val_int(30));

    ASSERT(obj.as.object->count == 2, "Should have 2 fields");

    Value *name = object_get(obj.as.object, "name");
    ASSERT(name != NULL && strcmp(name->as.string.data, "John") == 0,
           "name should be 'John'");

    Value *age = object_get(obj.as.object, "age");
    ASSERT(age != NULL && age->as.integer == 30, "age should be 30");

    ASSERT(object_has(obj.as.object, "name"), "Should have 'name'");
    ASSERT(!object_has(obj.as.object, "email"), "Should not have 'email'");

    arena_destroy(arena);
}

TEST(simple_component) {
    Arena *arena = arena_create(8192);

    /* Define a component and use it */
    VMResult result = run_source(arena,
        "<defcomp Greeting | name : string |>"
        "<span><output name></span>"
        "</defcomp>"
        "<Greeting name=\"World\">");

    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "<span>World</span>") == 0,
           "Should output component HTML");

    arena_destroy(arena);
}

TEST(component_multiple_props) {
    Arena *arena = arena_create(8192);

    /* Component with multiple props - both as strings */
    VMResult result = run_source(arena,
        "<defcomp Card | title : string |>"
        "<div><output title></div>"
        "</defcomp>"
        "<Card title=\"Items\">");

    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "<div>Items</div>") == 0,
           "Should output component with props");

    arena_destroy(arena);
}

TEST(component_default_slot_children) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<defcomp Layout | title |>"
        "<div class=\"layout\"><h1><output title></h1><main><children></main></div>"
        "</defcomp>"
        "<Layout title=\"Home\"><p>Hello</p></Layout>");

    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer,
                  "<div class=\"layout\"><h1>Home</h1><main><p>Hello</p></main></div>") == 0,
           "Should render default slot children");

    arena_destroy(arena);
}

TEST(component_default_slot_with_expression) {
    Arena *arena = arena_create(8192);

    VMResult result = run_source(arena,
        "<let message \"Welcome\">"
        "<defcomp Layout ||><section><children></section></defcomp>"
        "<Layout><p><output message></p></Layout>"
        "</let>");

    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "<section><p>Welcome</p></section>") == 0,
           "Should capture and render dynamic children output");

    arena_destroy(arena);
}

/* Mock fetch function for testing */
static Value mock_fetch(const DataRequirement *req, const Value *frame_slots, void *userdata) {
    (void)req;
    (void)frame_slots;
    VM *vm = (VM *)userdata;

    /* Return a mock array of objects */
    Value arr = val_array(vm, 2);

    Value obj1 = val_object(vm, 2);
    object_set(vm, obj1.as.object, "name", val_string(vm, "Apple", 5));
    object_set(vm, obj1.as.object, "price", val_int(100));
    array_push(vm, arr.as.array, obj1);

    Value obj2 = val_object(vm, 2);
    object_set(vm, obj2.as.object, "name", val_string(vm, "Banana", 6));
    object_set(vm, obj2.as.object, "price", val_int(50));
    array_push(vm, arr.as.array, obj2);

    return arr;
}

TEST(sql_data_fetch) {
    Arena *arena = arena_create(8192);

    /* Parse and compile with SQL */
    const char *src =
        "<let products select * from products>"
        "<for p in products><output p.name></for>"
        "</let>";

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
    ASSERT(mod->data_req_count == 1, "Should have 1 data requirement");

    VM *vm = vm_new(arena);
    vm_init(vm, mod);

    output_buffer[0] = '\0';
    output_len = 0;
    vm_set_output(vm, capture_output, NULL);
    vm_set_fetch(vm, mock_fetch, vm);

    VMResult result = vm_run(vm);
    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "AppleBanana") == 0, "Should output product names");

    arena_destroy(arena);
}

TEST(var_and_set) {
    Arena *arena = arena_create(8192);

    /* Test var and set - using self-closing syntax */
    VMResult result = run_source(arena,
        "<defcomp Counter ||>"
        "<var counter 0 />"
        "<output counter>"
        "<set counter 1 />"
        "<output counter>"
        "<set counter counter + 1 />"
        "<output counter>"
        "</defcomp>"
        "<Counter>");

    ASSERT(result == VM_OK, "Should succeed");
    ASSERT(strcmp(output_buffer, "012") == 0, "Should show var updates");

    arena_destroy(arena);
}

int main(void) {
    printf("=== VM Tests ===\n");

    RUN_TEST(simple_output);
    RUN_TEST(html_element);
    RUN_TEST(html_escape);
    RUN_TEST(integer_arithmetic);
    RUN_TEST(complex_arithmetic);
    RUN_TEST(string_concat);
    RUN_TEST(if_true);
    RUN_TEST(if_false);
    RUN_TEST(ternary_expression);
    RUN_TEST(match_statement);
    RUN_TEST(interface_and_export);
    RUN_TEST(macro_expansion);
    RUN_TEST(for_loop);
    RUN_TEST(for_loop_html);
    RUN_TEST(builtin_uppercase);
    RUN_TEST(builtin_trim);
    RUN_TEST(pipe_chain);
    RUN_TEST(elsif_single_branch);
    RUN_TEST(elsif_chain_late_match);
    RUN_TEST(builtin_many_args);
    RUN_TEST(opcode_and_or);
    RUN_TEST(opcode_concat);
    RUN_TEST(opcode_call_and_call_pipe);
    RUN_TEST(component_stack_cleanup);
    RUN_TEST(vm_step_basic);
    RUN_TEST(nested_let);
    RUN_TEST(value_constructors);
    RUN_TEST(value_is_truthy);
    RUN_TEST(value_equals);
    RUN_TEST(array_operations);
    RUN_TEST(object_operations);
    RUN_TEST(simple_component);
    RUN_TEST(component_multiple_props);
    RUN_TEST(component_default_slot_children);
    RUN_TEST(component_default_slot_with_expression);
    RUN_TEST(sql_data_fetch);
    RUN_TEST(var_and_set);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
