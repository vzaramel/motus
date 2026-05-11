/*
 * Parser tests
 */

#include <stdio.h>
#include <string.h>
#include "parser/parser.h"
#include "parser/ast.h"
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

TEST(simple_element) {
    const char *src = "<div>Hello</div>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");
    ASSERT(doc->type == NODE_DOCUMENT, "Expected document node");

    AstNode *div = doc->data.document.children;
    ASSERT(div != NULL, "Expected child");
    ASSERT(div->type == NODE_ELEMENT, "Expected element");
    ASSERT(strcmp(div->data.element.tag, "div") == 0, "Expected 'div' tag");

    arena_destroy(arena);
}

TEST(nested_elements) {
    const char *src = "<div><span>Text</span></div>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *div = doc->data.document.children;
    ASSERT(div != NULL, "Expected div");
    ASSERT(div->type == NODE_ELEMENT, "Expected element");

    AstNode *span = div->data.element.children;
    ASSERT(span != NULL, "Expected span");
    ASSERT(span->type == NODE_ELEMENT, "Expected element");
    ASSERT(strcmp(span->data.element.tag, "span") == 0, "Expected 'span' tag");

    arena_destroy(arena);
}

TEST(let_binding) {
    const char *src = "<let x 42><output x></let>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *let = doc->data.document.children;
    ASSERT(let != NULL, "Expected let");
    ASSERT(let->type == NODE_LET, "Expected LET node");
    ASSERT(strcmp(let->data.binding.name, "x") == 0, "Expected name 'x'");
    ASSERT(let->data.binding.value != NULL, "Expected value");
    ASSERT(let->data.binding.value->type == NODE_LITERAL, "Expected literal");

    arena_destroy(arena);
}

TEST(for_loop) {
    const char *src = "<for item in items><li><output item></li></for>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *loop = doc->data.document.children;
    ASSERT(loop != NULL, "Expected for");
    ASSERT(loop->type == NODE_FOR, "Expected FOR node");
    ASSERT(strcmp(loop->data.for_loop.item, "item") == 0, "Expected 'item'");

    arena_destroy(arena);
}

TEST(if_statement) {
    const char *src = "<if x gt 0><p>positive</p><else><p>zero or negative</p></if>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *if_node = doc->data.document.children;
    ASSERT(if_node != NULL, "Expected if");
    ASSERT(if_node->type == NODE_IF, "Expected IF node");
    ASSERT(if_node->data.if_stmt.condition != NULL, "Expected condition");
    ASSERT(if_node->data.if_stmt.then_body != NULL, "Expected then body");

    arena_destroy(arena);
}

TEST(output) {
    const char *src = "<output user.name>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *output = doc->data.document.children;
    ASSERT(output != NULL, "Expected output");
    ASSERT(output->type == NODE_OUTPUT, "Expected OUTPUT node");
    ASSERT(output->data.output.expr != NULL, "Expected expression");
    ASSERT(output->data.output.expr->type == NODE_MEMBER, "Expected member access");

    arena_destroy(arena);
}

TEST(pipe_expression) {
    const char *src = "<output name | uppercase | trim>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *output = doc->data.document.children;
    ASSERT(output != NULL, "Expected output");
    ASSERT(output->type == NODE_OUTPUT, "Expected OUTPUT node");
    ASSERT(output->data.output.expr->type == NODE_PIPE, "Expected pipe expression");

    arena_destroy(arena);
}

TEST(sql_in_let) {
    const char *src = "<let products select * from products where active = true limit 10></let>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *let = doc->data.document.children;
    ASSERT(let != NULL, "Expected let");
    ASSERT(let->type == NODE_LET, "Expected LET node");
    ASSERT(let->data.binding.sql != NULL, "Expected SQL query");
    ASSERT(let->data.binding.sql->type == NODE_SQL, "Expected SQL node");

    arena_destroy(arena);
}

TEST(sql_params_in_where) {
    const char *src = "<let productId 42><let product select * from products where id = :productId></let></let>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *outer = doc->data.document.children;
    ASSERT(outer != NULL, "Expected outer let");
    ASSERT(outer->type == NODE_LET, "Expected outer let node");

    AstNode *inner = outer->data.binding.body;
    ASSERT(inner != NULL, "Expected inner let");
    ASSERT(inner->type == NODE_LET, "Expected inner let node");
    ASSERT(inner->data.binding.sql != NULL, "Expected SQL query");

    AstNode *where = inner->data.binding.sql->data.sql.where;
    ASSERT(where != NULL, "Expected WHERE expression");
    ASSERT(where->type == NODE_BINARY, "Expected binary WHERE expression");
    ASSERT(where->data.binary.right != NULL, "Expected right side");
    ASSERT(where->data.binary.right->type == NODE_SQL_PARAM, "Expected SQL parameter node");
    ASSERT(strcmp(where->data.binary.right->data.sql_param.name, "productId") == 0,
           "Expected SQL parameter name");

    arena_destroy(arena);
}

TEST(match_statement) {
    const char *src =
        "<match status>"
        "<case \"loading\"><output \"L\"></case>"
        "<case \"ready\"><output \"R\"></case>"
        "<default><output \"D\"></default>"
        "</match>";

    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *match = doc->data.document.children;
    ASSERT(match != NULL, "Expected match");
    ASSERT(match->type == NODE_MATCH, "Expected MATCH node");
    ASSERT(match->data.match.value != NULL, "Expected match value");
    ASSERT(match->data.match.cases != NULL, "Expected match cases");
    ASSERT(match->data.match.cases->type == NODE_CASE, "Expected first case");
    ASSERT(match->data.match.cases->data.case_stmt.is_default == false, "Expected non-default first case");

    AstNode *last = match->data.match.cases;
    while (last->next) last = last->next;
    ASSERT(last->data.case_stmt.is_default == true, "Expected default last case");

    arena_destroy(arena);
}

TEST(export_statement) {
    const char *src = "<export default ProductPage>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *exp = doc->data.document.children;
    ASSERT(exp != NULL, "Expected export");
    ASSERT(exp->type == NODE_EXPORT, "Expected EXPORT node");
    ASSERT(exp->data.export.is_default == true, "Expected default export");
    ASSERT(exp->data.export.names != NULL, "Expected exported name");
    ASSERT(exp->data.export.names->type == NODE_IDENT, "Expected identifier");
    ASSERT(strcmp(exp->data.export.names->data.ident.name, "ProductPage") == 0,
           "Expected exported identifier");

    arena_destroy(arena);
}

TEST(interface_statement) {
    const char *src = "<interface ButtonProps | label : string, disabled := false | />";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *iface = doc->data.document.children;
    ASSERT(iface != NULL, "Expected interface");
    ASSERT(iface->type == NODE_INTERFACE, "Expected INTERFACE node");
    ASSERT(strcmp(iface->data.interface.name, "ButtonProps") == 0, "Expected interface name");
    ASSERT(iface->data.interface.props != NULL, "Expected interface props");

    arena_destroy(arena);
}

TEST(macro_statement) {
    const char *src =
        "<macro card |title|><div><output title><children></div></macro>"
        "<card title=\"T\"><p>Body</p></card>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *macro = doc->data.document.children;
    ASSERT(macro != NULL, "Expected macro");
    ASSERT(macro->type == NODE_MACRO, "Expected MACRO node");
    ASSERT(strcmp(macro->data.macro.name, "card") == 0, "Expected macro name");
    ASSERT(macro->data.macro.params != NULL, "Expected macro params");
    ASSERT(macro->data.macro.body != NULL, "Expected macro body");

    AstNode *call = macro->next;
    ASSERT(call != NULL, "Expected macro call element");
    ASSERT(call->type == NODE_ELEMENT, "Expected element for macro invocation");
    ASSERT(strcmp(call->data.element.tag, "card") == 0, "Expected invocation tag");

    arena_destroy(arena);
}

TEST(defcomp) {
    const char *src = "<defcomp Button | label : string, disabled := false |><button><output label></button></defcomp>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *comp = doc->data.document.children;
    ASSERT(comp != NULL, "Expected defcomp");
    ASSERT(comp->type == NODE_DEFCOMP, "Expected DEFCOMP node");
    ASSERT(strcmp(comp->data.defcomp.name, "Button") == 0, "Expected 'Button'");
    ASSERT(comp->data.defcomp.props != NULL, "Expected props");

    arena_destroy(arena);
}

TEST(import) {
    const char *src = "<import Button from \"./Button.mot\">";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *imp = doc->data.document.children;
    ASSERT(imp != NULL, "Expected import");
    ASSERT(imp->type == NODE_IMPORT, "Expected IMPORT node");
    ASSERT(strcmp(imp->data.import.from_path, "./Button.mot") == 0, "Expected path");

    arena_destroy(arena);
}

TEST(attributes) {
    const char *src = "<div class=\"container\" id=\"main\"></div>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *div = doc->data.document.children;
    ASSERT(div != NULL, "Expected div");
    ASSERT(div->data.element.attrs != NULL, "Expected attributes");

    AstNode *attr = div->data.element.attrs;
    ASSERT(strcmp(attr->data.attr.name, "class") == 0, "Expected 'class' attr");

    arena_destroy(arena);
}

TEST(attributes_with_keyword_name) {
    const char *src = "<label for=\"username\">User</label>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *label = doc->data.document.children;
    ASSERT(label != NULL, "Expected label");
    ASSERT(label->type == NODE_ELEMENT, "Expected element");
    ASSERT(label->data.element.attrs != NULL, "Expected attributes");
    ASSERT(strcmp(label->data.element.attrs->data.attr.name, "for") == 0,
           "Expected 'for' attribute name");

    arena_destroy(arena);
}

TEST(style_element) {
    const char *src = "<style>.button { color: red; }</style>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *style = doc->data.document.children;
    ASSERT(style != NULL, "Expected style node");
    ASSERT(style->type == NODE_STYLE, "Expected NODE_STYLE");
    ASSERT(style->data.embedded.code != NULL, "Expected code content");
    ASSERT(strstr(style->data.embedded.code, ".button") != NULL, "Expected CSS content");

    arena_destroy(arena);
}

TEST(script_element) {
    const char *src = "<script>function init() { console.log('hello'); }</script>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *script = doc->data.document.children;
    ASSERT(script != NULL, "Expected script node");
    ASSERT(script->type == NODE_SCRIPT, "Expected NODE_SCRIPT");
    ASSERT(script->data.embedded.code != NULL, "Expected code content");
    ASSERT(strstr(script->data.embedded.code, "function init") != NULL, "Expected JS content");

    arena_destroy(arena);
}

TEST(insert_statement) {
    const char *src = "<insert into contacts><button type=\"submit\">Add</button></insert>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *ins = doc->data.document.children;
    ASSERT(ins != NULL, "Expected insert node");
    ASSERT(ins->type == NODE_INSERT, "Expected NODE_INSERT");
    ASSERT(strcmp(ins->data.insert.target, "contacts") == 0, "Expected target 'contacts'");
    ASSERT(ins->data.insert.body != NULL, "Expected body");
    ASSERT(ins->data.insert.optimistic == true, "Expected optimistic=true by default");

    arena_destroy(arena);
}

TEST(insert_pessimistic) {
    const char *src = "<insert into contacts pessimistic><button type=\"submit\">Add</button></insert>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *ins = doc->data.document.children;
    ASSERT(ins != NULL, "Expected insert node");
    ASSERT(ins->type == NODE_INSERT, "Expected NODE_INSERT");
    ASSERT(ins->data.insert.optimistic == false, "Expected optimistic=false for pessimistic");

    arena_destroy(arena);
}

TEST(update_pessimistic) {
    const char *src = "<update contacts where id eq 1 pessimistic><button type=\"submit\">Save</button></update>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *upd = doc->data.document.children;
    ASSERT(upd != NULL, "Expected update node");
    ASSERT(upd->type == NODE_UPDATE, "Expected NODE_UPDATE");
    ASSERT(upd->data.update.optimistic == false, "Expected optimistic=false for pessimistic");

    arena_destroy(arena);
}

TEST(delete_pessimistic) {
    const char *src = "<delete from contacts where id eq 1 pessimistic><button type=\"submit\">Del</button></delete>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *del = doc->data.document.children;
    ASSERT(del != NULL, "Expected delete node");
    ASSERT(del->type == NODE_DELETE, "Expected NODE_DELETE");
    ASSERT(del->data.delete_stmt.optimistic == false, "Expected optimistic=false for pessimistic");

    arena_destroy(arena);
}

TEST(update_statement) {
    const char *src = "<update contacts where id eq 1><button type=\"submit\">Save</button></update>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *upd = doc->data.document.children;
    ASSERT(upd != NULL, "Expected update node");
    ASSERT(upd->type == NODE_UPDATE, "Expected NODE_UPDATE");
    ASSERT(strcmp(upd->data.update.target, "contacts") == 0, "Expected target 'contacts'");
    ASSERT(upd->data.update.where != NULL, "Expected where clause");

    arena_destroy(arena);
}

TEST(delete_statement) {
    const char *src = "<delete from contacts where id eq 1><button type=\"submit\">Remove</button></delete>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *del = doc->data.document.children;
    ASSERT(del != NULL, "Expected delete node");
    ASSERT(del->type == NODE_DELETE, "Expected NODE_DELETE");
    ASSERT(strcmp(del->data.delete_stmt.target, "contacts") == 0, "Expected target 'contacts'");
    ASSERT(del->data.delete_stmt.where != NULL, "Expected where clause");

    arena_destroy(arena);
}

TEST(bound_input) {
    const char *src = "<insert into contacts><input contact.name /></insert>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *ins = doc->data.document.children;
    ASSERT(ins != NULL, "Expected insert node");
    ASSERT(ins->type == NODE_INSERT, "Expected NODE_INSERT");

    AstNode *inp = ins->data.insert.body;
    ASSERT(inp != NULL, "Expected bound input");
    ASSERT(inp->type == NODE_BOUND_INPUT, "Expected NODE_BOUND_INPUT");
    ASSERT(strcmp(inp->data.bound_input.object_name, "contact") == 0, "Expected object 'contact'");
    ASSERT(strcmp(inp->data.bound_input.field_name, "name") == 0, "Expected field 'name'");

    arena_destroy(arena);
}

TEST(regular_input_in_mutation) {
    /* Regular <input type="text" /> should remain NODE_ELEMENT even inside mutation block */
    const char *src = "<insert into contacts><input type=\"text\" /></insert>";
    Arena *arena = arena_create(4096);
    Parser parser;
    parser_init(&parser, src, strlen(src), arena, NULL);

    AstNode *doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Expected document");

    AstNode *ins = doc->data.document.children;
    ASSERT(ins != NULL, "Expected insert node");

    AstNode *inp = ins->data.insert.body;
    ASSERT(inp != NULL, "Expected input element");
    ASSERT(inp->type == NODE_ELEMENT, "Regular input should be NODE_ELEMENT");
    ASSERT(strcmp(inp->data.element.tag, "input") == 0, "Expected 'input' tag");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Parser Tests ===\n");

    RUN_TEST(simple_element);
    RUN_TEST(nested_elements);
    RUN_TEST(let_binding);
    RUN_TEST(for_loop);
    RUN_TEST(if_statement);
    RUN_TEST(output);
    RUN_TEST(pipe_expression);
    RUN_TEST(sql_in_let);
    RUN_TEST(sql_params_in_where);
    RUN_TEST(match_statement);
    RUN_TEST(export_statement);
    RUN_TEST(interface_statement);
    RUN_TEST(macro_statement);
    RUN_TEST(defcomp);
    RUN_TEST(import);
    RUN_TEST(attributes);
    RUN_TEST(attributes_with_keyword_name);
    RUN_TEST(style_element);
    RUN_TEST(script_element);
    RUN_TEST(insert_statement);
    RUN_TEST(insert_pessimistic);
    RUN_TEST(update_statement);
    RUN_TEST(update_pessimistic);
    RUN_TEST(delete_statement);
    RUN_TEST(delete_pessimistic);
    RUN_TEST(bound_input);
    RUN_TEST(regular_input_in_mutation);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
