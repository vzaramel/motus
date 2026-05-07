/*
 * Compiler tests
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "compiler/compiler.h"
#include "compiler/bytecode.h"
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

/* Helper to parse, analyze, and compile */
static BytecodeModule *compile_source_mode(Arena *arena, const char *src, bool partial_eval) {
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);
    AstNode *doc = parser_parse(&parser);
    if (!doc) return NULL;

    Analyzer *a = analyzer_new(arena);
    analyzer_analyze(a, doc);
    if (!analyzer_ok(a)) return NULL;

    Compiler *c = compiler_new(arena, a);
    compiler_set_partial_eval(c, partial_eval);
    return compiler_compile(c, doc);
}

/* Baseline tests run with partial evaluation off for stable opcode assertions. */
static BytecodeModule *compile_source(Arena *arena, const char *src) {
    return compile_source_mode(arena, src, false);
}

TEST(simple_output) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<output \"Hello\">");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->main.code_len > 0, "Should have bytecode");

    /* Check for CONST and EMIT_TEXT opcodes */
    bool has_emit_text = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_EMIT_TEXT) {
            has_emit_text = true;
            break;
        }
    }
    ASSERT(has_emit_text, "Should have EMIT_TEXT opcode");

    arena_destroy(arena);
}

TEST(html_element) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<div>Hello</div>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for tag opcodes */
    bool has_tag_open = false;
    bool has_tag_close = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_EMIT_TAG_OPEN) has_tag_open = true;
        if (mod->main.code[i] == BC_EMIT_TAG_CLOSE) has_tag_close = true;
    }
    ASSERT(has_tag_open, "Should have TAG_OPEN opcode");
    ASSERT(has_tag_close, "Should have TAG_CLOSE opcode");

    arena_destroy(arena);
}

TEST(let_binding) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<let x 42><output x></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for INT opcode (42 fits in i16) */
    bool has_int = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_INT) {
            has_int = true;
            break;
        }
    }
    ASSERT(has_int, "Should have INT opcode for literal 42");

    arena_destroy(arena);
}

TEST(arithmetic) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<let x 1 + 2><output x></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for ADD opcode */
    bool has_add = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_ADD) {
            has_add = true;
            break;
        }
    }
    ASSERT(has_add, "Should have ADD opcode");

    arena_destroy(arena);
}

TEST(if_statement) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let x true><if x><output \"yes\"><else><output \"no\"></if></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for JUMP_IF_FALSE opcode */
    bool has_jump = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_JUMP_IF_FALSE) {
            has_jump = true;
            break;
        }
    }
    ASSERT(has_jump, "Should have JUMP_IF_FALSE opcode");

    arena_destroy(arena);
}

TEST(ternary_expression) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<let x if true then 1 else 2><output x></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_jump_if_false = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_JUMP_IF_FALSE) {
            has_jump_if_false = true;
            break;
        }
    }
    ASSERT(has_jump_if_false, "Should have conditional jump for ternary");

    arena_destroy(arena);
}

TEST(match_statement) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let status \"ready\">"
        "<match status>"
        "<case \"loading\"><output \"L\"></case>"
        "<case \"ready\"><output \"R\"></case>"
        "<default><output \"D\"></default>"
        "</match>"
        "</let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_eq = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_EQ) {
            has_eq = true;
            break;
        }
    }
    ASSERT(has_eq, "Should compare match value to case patterns");

    arena_destroy(arena);
}

TEST(interface_and_export) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<interface CardProps | title : string | />"
        "<defcomp Card | title : string |><div><output title></div></defcomp>"
        "<export default Card>"
        "<Card title=\"Items\">");
    ASSERT(mod != NULL, "Compilation should succeed with interface/export metadata");

    arena_destroy(arena);
}

TEST(macro_expansion) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let items [1, 2]>"
        "<macro product-list |items|><for item in items><output item></for></macro>"
        "<product-list items />"
        "</let>");
    ASSERT(mod != NULL, "Compilation should succeed for macro expansion");

    arena_destroy(arena);
}

TEST(for_loop) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let items [1, 2, 3]><for item in items><output item></for></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for iteration opcodes */
    bool has_iter_start = false;
    bool has_iter_next = false;
    bool has_iter_end = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_ITER_START) has_iter_start = true;
        if (mod->main.code[i] == BC_ITER_NEXT) has_iter_next = true;
        if (mod->main.code[i] == BC_ITER_END) has_iter_end = true;
    }
    ASSERT(has_iter_start, "Should have ITER_START opcode");
    ASSERT(has_iter_next, "Should have ITER_NEXT opcode");
    ASSERT(has_iter_end, "Should have ITER_END opcode");

    arena_destroy(arena);
}

TEST(pipe_expression) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<output \"hello\" | uppercase>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Check for CALL_BUILTIN opcode */
    bool has_call_builtin = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_CALL_BUILTIN) {
            has_call_builtin = true;
            break;
        }
    }
    ASSERT(has_call_builtin, "Should have CALL_BUILTIN opcode for pipe");

    arena_destroy(arena);
}

TEST(partial_eval_folds_arithmetic) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source_mode(arena, "<output 1 + 2 * 3>", true);
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_mul = false;
    bool has_add = false;
    bool has_int = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_MUL) has_mul = true;
        if (mod->main.code[i] == BC_ADD) has_add = true;
        if (mod->main.code[i] == BC_INT) has_int = true;
    }

    ASSERT(!has_mul, "Should fold multiplication");
    ASSERT(!has_add, "Should fold addition");
    ASSERT(has_int, "Should emit folded integer literal");

    arena_destroy(arena);
}

TEST(partial_eval_folds_pipe_builtin) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source_mode(arena, "<output \" hello \" | trim | uppercase>", true);
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_call_builtin = false;
    bool has_const = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_CALL_BUILTIN) has_call_builtin = true;
        if (mod->main.code[i] == BC_CONST) has_const = true;
    }

    ASSERT(!has_call_builtin, "Should fold pure builtin pipe stages");
    ASSERT(has_const, "Should emit folded string constant");

    arena_destroy(arena);
}

TEST(partial_eval_folds_ternary) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source_mode(arena, "<output if true then 7 else 9>", true);
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_jump = false;
    bool has_int = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_JUMP_IF_FALSE || mod->main.code[i] == BC_JUMP) has_jump = true;
        if (mod->main.code[i] == BC_INT) has_int = true;
    }

    ASSERT(!has_jump, "Should fold ternary condition and remove jumps");
    ASSERT(has_int, "Should emit chosen ternary branch as literal");

    arena_destroy(arena);
}

TEST(builtins_registered) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<output \"test\">");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->builtin_count > 0, "Should have registered builtins");

    /* Check for specific builtins */
    ASSERT(bytecode_find_builtin(mod, "uppercase") >= 0, "uppercase should be registered");
    ASSERT(bytecode_find_builtin(mod, "trim") >= 0, "trim should be registered");
    ASSERT(bytecode_find_builtin(mod, "length") >= 0, "length should be registered");

    arena_destroy(arena);
}

TEST(string_constants) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<div class=\"container\"></div>");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->string_count > 0, "Should have string constants");

    /* String interning - same string should be reused */
    uint16_t idx1 = bytecode_add_string(mod, "test", 4);
    uint16_t idx2 = bytecode_add_string(mod, "test", 4);
    ASSERT(idx1 == idx2, "Same strings should be interned");

    arena_destroy(arena);
}

TEST(disassemble) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena, "<output 42>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Just make sure disassemble doesn't crash */
    printf("\n");
    bytecode_disassemble(mod);

    arena_destroy(arena);
}

TEST(dependency_tracking) {
    Arena *arena = arena_create(4096);

    /* Test that dependencies are recorded when outputting a variable */
    BytecodeModule *mod = compile_source(arena,
        "<let user {name: \"John\"}><output user.name></let>");
    ASSERT(mod != NULL, "Compilation should succeed");

    /* Should have recorded the dependency on user.name */
    ASSERT(mod->dep_count > 0, "Should have recorded dependencies");

    /* Check for BC_DEP_START opcode in bytecode */
    bool has_dep_start = false;
    bool has_dep_end = false;
    for (uint32_t i = 0; i < mod->main.code_len; i++) {
        if (mod->main.code[i] == BC_DEP_START) has_dep_start = true;
        if (mod->main.code[i] == BC_DEP_END) has_dep_end = true;
    }
    ASSERT(has_dep_start, "Should have BC_DEP_START opcode");
    ASSERT(has_dep_end, "Should have BC_DEP_END opcode");

    arena_destroy(arena);
}

TEST(direct_var_binding_marker) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let counterValue 1><output counterValue></let>");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->dep_count > 0, "Should have dependency markers");

    bool has_var_marker = false;
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        if (strcmp(mod->deps[i].path, "@bind:counterValue") == 0) {
            has_var_marker = true;
            break;
        }
    }
    ASSERT(has_var_marker, "Should include direct variable marker dependency");

    arena_destroy(arena);
}

TEST(direct_member_binding_marker) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let user {name: \"John\"}><output user.name></let>");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->dep_count > 0, "Should have dependency markers");

    bool has_member_marker = false;
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        if (strcmp(mod->deps[i].path, "@bind:user.name") == 0) {
            has_member_marker = true;
            break;
        }
    }
    ASSERT(has_member_marker, "Should include direct member marker dependency");

    arena_destroy(arena);
}

TEST(set_target_marker) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<defcomp MarkerDemo ||>"
        "<var user {name: \"A\"} />"
        "<var counter 0 />"
        "<set user.name \"B\" />"
        "<set counter counter + 1 />"
        "</defcomp>"
        "<MarkerDemo>");
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_set_user_name = false;
    bool has_set_counter = false;
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        if (strcmp(mod->deps[i].path, "@set:user.name") == 0) {
            has_set_user_name = true;
        }
        if (strcmp(mod->deps[i].path, "@set:counter") == 0) {
            has_set_counter = true;
        }
    }

    ASSERT(has_set_user_name, "Should include mutable target marker for user.name");
    ASSERT(has_set_counter, "Should include mutable target marker for counter");

    arena_destroy(arena);
}

TEST(reactive_plan_marker) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<defcomp MarkerPlan ||>"
        "<var user {name: \"A\"} />"
        "<var counter 0 />"
        "<output counter>"
        "<output user.name>"
        "<output counter + 1>"
        "<div title=user.name></div>"
        "<div data-note=user.name + \"!\"></div>"
        "<set user.name \"B\" />"
        "<set counter counter + 1 />"
        "</defcomp>"
        "<MarkerPlan>");
    ASSERT(mod != NULL, "Compilation should succeed");

    bool has_counter_plan = false;
    bool has_user_plan = false;
    bool has_bindattr_user_title = false;
    bool has_planattr_user_title = false;
    bool has_exprbind = false;
    bool has_exprdep_counter = false;
    bool has_planexpr_counter = false;
    bool has_exprattr = false;
    for (uint32_t i = 0; i < mod->dep_count; i++) {
        if (strcmp(mod->deps[i].path, "@plan:counter|counter") == 0) {
            has_counter_plan = true;
        }
        if (strcmp(mod->deps[i].path, "@plan:user.name|user.name") == 0) {
            has_user_plan = true;
        }
        if (strncmp(mod->deps[i].path, "@exprbind:", 10) == 0 &&
            strstr(mod->deps[i].path, "|P:counter;I:1;O:add;") != NULL) {
            has_exprbind = true;
        }
        if (strncmp(mod->deps[i].path, "@exprdep:", 9) == 0 &&
            strstr(mod->deps[i].path, "|counter") != NULL) {
            has_exprdep_counter = true;
        }
        if (strncmp(mod->deps[i].path, "@planexpr:counter|", 18) == 0) {
            has_planexpr_counter = true;
        }
        if (strncmp(mod->deps[i].path, "@bindattr:", 10) == 0 &&
            strstr(mod->deps[i].path, "|title|user.name") != NULL) {
            has_bindattr_user_title = true;
        }
        if (strncmp(mod->deps[i].path, "@planattr:user.name|", 20) == 0 &&
            strstr(mod->deps[i].path, "|title|user.name") != NULL) {
            has_planattr_user_title = true;
        }
        if (strncmp(mod->deps[i].path, "@exprattr:", 10) == 0 &&
            strstr(mod->deps[i].path, "|data-note|") != NULL &&
            strstr(mod->deps[i].path, "|P:user.name;S:%21;O:add;") != NULL) {
            has_exprattr = true;
        }
    }

    ASSERT(has_counter_plan, "Should include explicit reactive plan for counter");
    ASSERT(has_user_plan, "Should include explicit reactive plan for user.name");
    ASSERT(has_exprbind, "Should include expression output marker for counter + 1");
    ASSERT(has_exprdep_counter, "Should include expression dependency marker for counter");
    ASSERT(has_planexpr_counter, "Should include planexpr mapping for counter expression updates");
    ASSERT(has_bindattr_user_title, "Should include bindattr marker for title=user.name");
    ASSERT(has_planattr_user_title, "Should include explicit planattr marker for title=user.name");
    ASSERT(has_exprattr, "Should include expression attr marker for data-note=user.name + '!'");

    arena_destroy(arena);
}

TEST(sql_query_metadata) {
    Arena *arena = arena_create(4096);

    BytecodeModule *mod = compile_source(arena,
        "<let productId 42>"
        "<let product select * from products where id = :productId></let>"
        "</let>");
    ASSERT(mod != NULL, "Compilation should succeed");
    ASSERT(mod->data_req_count == 1, "Should have one data requirement");

    DataRequirement *req = &mod->data_reqs[0];
    ASSERT(req->query_ref == 0, "Expected query_ref to match index");
    ASSERT(req->signature != 0, "Expected non-zero query signature");
    ASSERT(req->param_count == 1, "Expected one SQL parameter");
    ASSERT(strcmp(req->param_names[0], "productId") == 0, "Expected parameter name");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Compiler Tests ===\n");

    RUN_TEST(simple_output);
    RUN_TEST(html_element);
    RUN_TEST(let_binding);
    RUN_TEST(arithmetic);
    RUN_TEST(if_statement);
    RUN_TEST(ternary_expression);
    RUN_TEST(match_statement);
    RUN_TEST(interface_and_export);
    RUN_TEST(macro_expansion);
    RUN_TEST(for_loop);
    RUN_TEST(pipe_expression);
    RUN_TEST(partial_eval_folds_arithmetic);
    RUN_TEST(partial_eval_folds_pipe_builtin);
    RUN_TEST(partial_eval_folds_ternary);
    RUN_TEST(builtins_registered);
    RUN_TEST(string_constants);
    RUN_TEST(disassemble);
    RUN_TEST(dependency_tracking);
    RUN_TEST(direct_var_binding_marker);
    RUN_TEST(direct_member_binding_marker);
    RUN_TEST(set_target_marker);
    RUN_TEST(reactive_plan_marker);
    RUN_TEST(sql_query_metadata);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
