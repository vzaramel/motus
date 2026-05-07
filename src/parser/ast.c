/*
 * AST construction and utility functions
 */

#include "ast.h"
#include <stdio.h>
#include <string.h>

const char *node_type_name(NodeType type) {
    switch (type) {
        case NODE_DOCUMENT: return "DOCUMENT";
        case NODE_ELEMENT: return "ELEMENT";
        case NODE_TEXT: return "TEXT";
        case NODE_COMMENT: return "COMMENT";
        case NODE_DOCTYPE: return "DOCTYPE";
        case NODE_LET: return "LET";
        case NODE_VAR: return "VAR";
        case NODE_SET: return "SET";
        case NODE_OUTPUT: return "OUTPUT";
        case NODE_IF: return "IF";
        case NODE_ELSIF: return "ELSIF";
        case NODE_ELSE: return "ELSE";
        case NODE_FOR: return "FOR";
        case NODE_MATCH: return "MATCH";
        case NODE_CASE: return "CASE";
        case NODE_DEFCOMP: return "DEFCOMP";
        case NODE_MACRO: return "MACRO";
        case NODE_CHILDREN: return "CHILDREN";
        case NODE_IMPORT: return "IMPORT";
        case NODE_EXPORT: return "EXPORT";
        case NODE_INTERFACE: return "INTERFACE";
        case NODE_REQUIRE_AUTH: return "REQUIRE_AUTH";
        case NODE_PROP_DEF: return "PROP_DEF";
        case NODE_SLOT_DEF: return "SLOT_DEF";
        case NODE_SLOT_FILL: return "SLOT_FILL";
        case NODE_STYLE: return "STYLE";
        case NODE_SCRIPT: return "SCRIPT";
        case NODE_LITERAL: return "LITERAL";
        case NODE_IDENT: return "IDENT";
        case NODE_BINARY: return "BINARY";
        case NODE_UNARY: return "UNARY";
        case NODE_CALL: return "CALL";
        case NODE_PIPE: return "PIPE";
        case NODE_MEMBER: return "MEMBER";
        case NODE_INDEX: return "INDEX";
        case NODE_TERNARY: return "TERNARY";
        case NODE_ARRAY: return "ARRAY";
        case NODE_OBJECT: return "OBJECT";
        case NODE_RANGE: return "RANGE";
        case NODE_SQL: return "SQL";
        case NODE_SQL_SELECT: return "SQL_SELECT";
        case NODE_SQL_FROM: return "SQL_FROM";
        case NODE_SQL_JOIN: return "SQL_JOIN";
        case NODE_SQL_WHERE: return "SQL_WHERE";
        case NODE_SQL_ORDER: return "SQL_ORDER";
        case NODE_SQL_LIMIT: return "SQL_LIMIT";
        case NODE_SQL_OFFSET: return "SQL_OFFSET";
        case NODE_SQL_COLUMN: return "SQL_COLUMN";
        case NODE_SQL_PARAM: return "SQL_PARAM";
        case NODE_ATTR: return "ATTR";
    }
    return "UNKNOWN";
}

AstNode *ast_node_new(Arena *arena, NodeType type, int line, int col) {
    AstNode *node = arena_calloc(arena, 1, sizeof(AstNode));
    node->type = type;
    node->line = line;
    node->column = col;
    node->source_path = NULL;
    node->next = NULL;
    node->resolved_type = NULL;
    node->scope = NULL;
    node->deps = NULL;
    return node;
}

void ast_set_source_path_recursive(AstNode *node, const char *source_path) {
    for (AstNode *cur = node; cur; cur = cur->next) {
        cur->source_path = source_path;

        switch (cur->type) {
            case NODE_DOCUMENT:
                ast_set_source_path_recursive(cur->data.document.children, source_path);
                break;
            case NODE_ELEMENT:
                ast_set_source_path_recursive(cur->data.element.attrs, source_path);
                ast_set_source_path_recursive(cur->data.element.children, source_path);
                break;
            case NODE_ATTR:
                ast_set_source_path_recursive(cur->data.attr.value, source_path);
                break;
            case NODE_LET:
            case NODE_VAR:
                ast_set_source_path_recursive(cur->data.binding.value, source_path);
                ast_set_source_path_recursive(cur->data.binding.sql, source_path);
                ast_set_source_path_recursive(cur->data.binding.body, source_path);
                break;
            case NODE_SET:
                ast_set_source_path_recursive(cur->data.set.target, source_path);
                ast_set_source_path_recursive(cur->data.set.value, source_path);
                break;
            case NODE_OUTPUT:
                ast_set_source_path_recursive(cur->data.output.expr, source_path);
                break;
            case NODE_IF:
            case NODE_ELSIF:
                ast_set_source_path_recursive(cur->data.if_stmt.condition, source_path);
                ast_set_source_path_recursive(cur->data.if_stmt.then_body, source_path);
                ast_set_source_path_recursive(cur->data.if_stmt.else_branch, source_path);
                break;
            case NODE_ELSE:
                ast_set_source_path_recursive(cur->data.else_stmt.body, source_path);
                break;
            case NODE_FOR:
                ast_set_source_path_recursive(cur->data.for_loop.iterable, source_path);
                ast_set_source_path_recursive(cur->data.for_loop.body, source_path);
                break;
            case NODE_MATCH:
                ast_set_source_path_recursive(cur->data.match.value, source_path);
                ast_set_source_path_recursive(cur->data.match.cases, source_path);
                break;
            case NODE_CASE:
                ast_set_source_path_recursive(cur->data.case_stmt.pattern, source_path);
                ast_set_source_path_recursive(cur->data.case_stmt.body, source_path);
                break;
            case NODE_DEFCOMP:
                ast_set_source_path_recursive(cur->data.defcomp.props, source_path);
                ast_set_source_path_recursive(cur->data.defcomp.slots, source_path);
                ast_set_source_path_recursive(cur->data.defcomp.body, source_path);
                break;
            case NODE_MACRO:
                ast_set_source_path_recursive(cur->data.macro.params, source_path);
                ast_set_source_path_recursive(cur->data.macro.body, source_path);
                break;
            case NODE_PROP_DEF:
                ast_set_source_path_recursive(cur->data.prop_def.default_val, source_path);
                break;
            case NODE_SLOT_FILL:
                ast_set_source_path_recursive(cur->data.slot_fill.params, source_path);
                ast_set_source_path_recursive(cur->data.slot_fill.content, source_path);
                break;
            case NODE_IMPORT:
                ast_set_source_path_recursive(cur->data.import.names, source_path);
                break;
            case NODE_EXPORT:
                ast_set_source_path_recursive(cur->data.export.names, source_path);
                break;
            case NODE_INTERFACE:
                ast_set_source_path_recursive(cur->data.interface.props, source_path);
                break;
            case NODE_BINARY:
                ast_set_source_path_recursive(cur->data.binary.left, source_path);
                ast_set_source_path_recursive(cur->data.binary.right, source_path);
                break;
            case NODE_UNARY:
                ast_set_source_path_recursive(cur->data.unary.operand, source_path);
                break;
            case NODE_CALL:
                ast_set_source_path_recursive(cur->data.call.args, source_path);
                break;
            case NODE_PIPE:
                ast_set_source_path_recursive(cur->data.pipe.input, source_path);
                ast_set_source_path_recursive(cur->data.pipe.stages, source_path);
                break;
            case NODE_MEMBER:
                ast_set_source_path_recursive(cur->data.member.object, source_path);
                break;
            case NODE_INDEX:
                ast_set_source_path_recursive(cur->data.index.object, source_path);
                ast_set_source_path_recursive(cur->data.index.index, source_path);
                break;
            case NODE_TERNARY:
                ast_set_source_path_recursive(cur->data.ternary.condition, source_path);
                ast_set_source_path_recursive(cur->data.ternary.then_expr, source_path);
                ast_set_source_path_recursive(cur->data.ternary.else_expr, source_path);
                break;
            case NODE_ARRAY:
                ast_set_source_path_recursive(cur->data.array.elements, source_path);
                break;
            case NODE_OBJECT:
                ast_set_source_path_recursive(cur->data.object.pairs, source_path);
                break;
            case NODE_RANGE:
                ast_set_source_path_recursive(cur->data.range.start, source_path);
                ast_set_source_path_recursive(cur->data.range.end, source_path);
                break;
            case NODE_SQL:
                ast_set_source_path_recursive(cur->data.sql.columns, source_path);
                ast_set_source_path_recursive(cur->data.sql.from, source_path);
                ast_set_source_path_recursive(cur->data.sql.joins, source_path);
                ast_set_source_path_recursive(cur->data.sql.where, source_path);
                ast_set_source_path_recursive(cur->data.sql.order, source_path);
                ast_set_source_path_recursive(cur->data.sql.limit, source_path);
                ast_set_source_path_recursive(cur->data.sql.offset, source_path);
                break;
            case NODE_SQL_COLUMN:
                ast_set_source_path_recursive(cur->data.sql_column.expr, source_path);
                break;
            case NODE_SQL_JOIN:
                ast_set_source_path_recursive(cur->data.sql_table.on_condition, source_path);
                break;
            case NODE_SQL_ORDER:
                ast_set_source_path_recursive(cur->data.sql_order.expr, source_path);
                break;
            case NODE_CHILDREN:
            case NODE_SLOT_DEF:
            case NODE_STYLE:
            case NODE_SCRIPT:
            case NODE_LITERAL:
            case NODE_IDENT:
            case NODE_SQL_SELECT:
            case NODE_SQL_FROM:
            case NODE_SQL_WHERE:
            case NODE_SQL_LIMIT:
            case NODE_SQL_OFFSET:
            case NODE_SQL_PARAM:
            case NODE_REQUIRE_AUTH:
            case NODE_COMMENT:
            case NODE_DOCTYPE:
            case NODE_TEXT:
                break;
        }
    }
}

AstNode *ast_document(Arena *arena, AstNode *children) {
    AstNode *node = ast_node_new(arena, NODE_DOCUMENT, 1, 1);
    node->data.document.children = children;
    return node;
}

AstNode *ast_element(Arena *arena, const char *tag, AstNode *attrs, AstNode *children, bool self_closing, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_ELEMENT, line, col);
    node->data.element.tag = arena_strdup(arena, tag);
    node->data.element.attrs = attrs;
    node->data.element.children = children;
    node->data.element.self_closing = self_closing;
    return node;
}

AstNode *ast_text(Arena *arena, const char *content, size_t length, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_TEXT, line, col);
    node->data.text.content = arena_strndup(arena, content, length);
    node->data.text.length = length;
    return node;
}

AstNode *ast_comment(Arena *arena, const char *content, size_t length, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_COMMENT, line, col);
    node->data.text.content = arena_strndup(arena, content, length);
    node->data.text.length = length;
    return node;
}

AstNode *ast_attr(Arena *arena, const char *name, AstNode *value, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_ATTR, line, col);
    node->data.attr.name = arena_strdup(arena, name);
    node->data.attr.value = value;
    return node;
}

AstNode *ast_let(Arena *arena, const char *name, AstNode *value, AstNode *sql, AstNode *body, bool dynamic, bool single, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LET, line, col);
    node->data.binding.name = arena_strdup(arena, name);
    node->data.binding.value = value;
    node->data.binding.sql = sql;
    node->data.binding.body = body;
    node->data.binding.dynamic = dynamic;
    node->data.binding.single = single;
    return node;
}

AstNode *ast_var(Arena *arena, const char *name, AstNode *value, AstNode *sql, bool dynamic, bool single, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_VAR, line, col);
    node->data.binding.name = arena_strdup(arena, name);
    node->data.binding.value = value;
    node->data.binding.sql = sql;
    node->data.binding.body = NULL;  /* var doesn't have scoped body */
    node->data.binding.dynamic = dynamic;
    node->data.binding.single = single;
    return node;
}

AstNode *ast_set(Arena *arena, AstNode *target, AstNode *value, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SET, line, col);
    node->data.set.target = target;
    node->data.set.value = value;
    return node;
}

AstNode *ast_output(Arena *arena, AstNode *expr, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_OUTPUT, line, col);
    node->data.output.expr = expr;
    return node;
}

AstNode *ast_if(Arena *arena, AstNode *condition, AstNode *then_body, AstNode *else_branch, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_IF, line, col);
    node->data.if_stmt.condition = condition;
    node->data.if_stmt.then_body = then_body;
    node->data.if_stmt.else_branch = else_branch;
    return node;
}

AstNode *ast_elsif(Arena *arena, AstNode *condition, AstNode *then_body, AstNode *else_branch, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_ELSIF, line, col);
    node->data.if_stmt.condition = condition;
    node->data.if_stmt.then_body = then_body;
    node->data.if_stmt.else_branch = else_branch;
    return node;
}

AstNode *ast_else(Arena *arena, AstNode *body, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_ELSE, line, col);
    node->data.else_stmt.body = body;
    return node;
}

AstNode *ast_for(Arena *arena, const char *item, const char *index, AstNode *iterable, AstNode *body, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_FOR, line, col);
    node->data.for_loop.item = arena_strdup(arena, item);
    node->data.for_loop.index = index ? arena_strdup(arena, index) : NULL;
    node->data.for_loop.iterable = iterable;
    node->data.for_loop.body = body;
    return node;
}

AstNode *ast_match(Arena *arena, AstNode *value, AstNode *cases, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_MATCH, line, col);
    node->data.match.value = value;
    node->data.match.cases = cases;
    return node;
}

AstNode *ast_case(Arena *arena, AstNode *pattern, AstNode *body, bool is_default, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_CASE, line, col);
    node->data.case_stmt.pattern = pattern;
    node->data.case_stmt.body = body;
    node->data.case_stmt.is_default = is_default;
    return node;
}

AstNode *ast_defcomp(Arena *arena, const char *name, AstNode *props, AstNode *slots, AstNode *body, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_DEFCOMP, line, col);
    node->data.defcomp.name = arena_strdup(arena, name);
    node->data.defcomp.props = props;
    node->data.defcomp.slots = slots;
    node->data.defcomp.body = body;
    return node;
}

AstNode *ast_macro(Arena *arena, const char *name, AstNode *params, AstNode *body, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_MACRO, line, col);
    node->data.macro.name = arena_strdup(arena, name);
    node->data.macro.params = params;
    node->data.macro.body = body;
    return node;
}

AstNode *ast_prop_def(Arena *arena, const char *name, const char *type_name, AstNode *default_val, bool required, bool infer_type, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_PROP_DEF, line, col);
    node->data.prop_def.name = arena_strdup(arena, name);
    node->data.prop_def.type_name = type_name ? arena_strdup(arena, type_name) : NULL;
    node->data.prop_def.default_val = default_val;
    node->data.prop_def.required = required;
    node->data.prop_def.infer_type = infer_type;
    return node;
}

AstNode *ast_slot_def(Arena *arena, const char *name, const char *interface, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SLOT_DEF, line, col);
    node->data.slot_def.name = arena_strdup(arena, name);
    node->data.slot_def.interface = interface ? arena_strdup(arena, interface) : NULL;
    return node;
}

AstNode *ast_slot_fill(Arena *arena, const char *slot_name, AstNode *params, AstNode *content, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SLOT_FILL, line, col);
    node->data.slot_fill.slot_name = arena_strdup(arena, slot_name);
    node->data.slot_fill.params = params;
    node->data.slot_fill.content = content;
    return node;
}

AstNode *ast_children(Arena *arena, int line, int col) {
    return ast_node_new(arena, NODE_CHILDREN, line, col);
}

AstNode *ast_import(Arena *arena, AstNode *names, const char *from_path, bool is_external, bool is_dynamic, const char *as_name, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_IMPORT, line, col);
    node->data.import.names = names;
    node->data.import.from_path = arena_strdup(arena, from_path);
    node->data.import.is_external = is_external;
    node->data.import.is_dynamic = is_dynamic;
    node->data.import.as_name = as_name ? arena_strdup(arena, as_name) : NULL;
    return node;
}

AstNode *ast_export(Arena *arena, AstNode *names, bool is_default, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_EXPORT, line, col);
    node->data.export.names = names;
    node->data.export.is_default = is_default;
    return node;
}

AstNode *ast_style(Arena *arena, const char *code, size_t length, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_STYLE, line, col);
    node->data.embedded.code = arena_strndup(arena, code, length);
    node->data.embedded.code_len = length;
    return node;
}

AstNode *ast_script(Arena *arena, const char *code, size_t length, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SCRIPT, line, col);
    node->data.embedded.code = arena_strndup(arena, code, length);
    node->data.embedded.code_len = length;
    return node;
}

AstNode *ast_literal_number(Arena *arena, double value, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LITERAL, line, col);
    node->data.literal.lit_type = LIT_NUMBER;
    node->data.literal.v.number = value;
    return node;
}

AstNode *ast_literal_int(Arena *arena, int64_t value, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LITERAL, line, col);
    node->data.literal.lit_type = LIT_INT;
    node->data.literal.v.integer = value;
    return node;
}

AstNode *ast_literal_string(Arena *arena, const char *value, size_t length, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LITERAL, line, col);
    node->data.literal.lit_type = LIT_STRING;
    node->data.literal.v.string.value = arena_strndup(arena, value, length);
    node->data.literal.v.string.length = length;
    return node;
}

AstNode *ast_literal_bool(Arena *arena, bool value, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LITERAL, line, col);
    node->data.literal.lit_type = LIT_BOOL;
    node->data.literal.v.boolean = value;
    return node;
}

AstNode *ast_literal_null(Arena *arena, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_LITERAL, line, col);
    node->data.literal.lit_type = LIT_NULL;
    return node;
}

AstNode *ast_ident(Arena *arena, const char *name, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_IDENT, line, col);
    node->data.ident.name = arena_strdup(arena, name);
    return node;
}

AstNode *ast_binary(Arena *arena, OpType op, AstNode *left, AstNode *right, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_BINARY, line, col);
    node->data.binary.op = op;
    node->data.binary.left = left;
    node->data.binary.right = right;
    return node;
}

AstNode *ast_unary(Arena *arena, OpType op, AstNode *operand, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_UNARY, line, col);
    node->data.unary.op = op;
    node->data.unary.operand = operand;
    return node;
}

AstNode *ast_call(Arena *arena, const char *name, AstNode *args, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_CALL, line, col);
    node->data.call.name = arena_strdup(arena, name);
    node->data.call.args = args;
    return node;
}

AstNode *ast_pipe(Arena *arena, AstNode *input, AstNode *stages, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_PIPE, line, col);
    node->data.pipe.input = input;
    node->data.pipe.stages = stages;
    return node;
}

AstNode *ast_member(Arena *arena, AstNode *object, const char *member, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_MEMBER, line, col);
    node->data.member.object = object;
    node->data.member.member = arena_strdup(arena, member);
    return node;
}

AstNode *ast_index(Arena *arena, AstNode *object, AstNode *index_expr, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_INDEX, line, col);
    node->data.index.object = object;
    node->data.index.index = index_expr;
    return node;
}

AstNode *ast_ternary(Arena *arena, AstNode *condition, AstNode *then_expr, AstNode *else_expr, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_TERNARY, line, col);
    node->data.ternary.condition = condition;
    node->data.ternary.then_expr = then_expr;
    node->data.ternary.else_expr = else_expr;
    return node;
}

AstNode *ast_array(Arena *arena, AstNode *elements, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_ARRAY, line, col);
    node->data.array.elements = elements;
    return node;
}

AstNode *ast_object(Arena *arena, AstNode *pairs, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_OBJECT, line, col);
    node->data.object.pairs = pairs;
    return node;
}

AstNode *ast_range(Arena *arena, AstNode *start, AstNode *end, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_RANGE, line, col);
    node->data.range.start = start;
    node->data.range.end = end;
    return node;
}

AstNode *ast_sql(Arena *arena, AstNode *columns, AstNode *from, AstNode *joins, AstNode *where, AstNode *order, AstNode *limit, AstNode *offset, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SQL, line, col);
    node->data.sql.columns = columns;
    node->data.sql.from = from;
    node->data.sql.joins = joins;
    node->data.sql.where = where;
    node->data.sql.order = order;
    node->data.sql.limit = limit;
    node->data.sql.offset = offset;
    return node;
}

AstNode *ast_sql_param(Arena *arena, const char *name, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_SQL_PARAM, line, col);
    node->data.sql_param.name = arena_strdup(arena, name);
    return node;
}

AstNode *ast_require_auth(Arena *arena, const char *role, int line, int col) {
    AstNode *node = ast_node_new(arena, NODE_REQUIRE_AUTH, line, col);
    node->data.require_auth.role = role ? arena_strdup(arena, role) : NULL;
    return node;
}

void ast_append_child(AstNode *parent, AstNode *child) {
    if (!parent || !child) return;

    AstNode **children = NULL;

    switch (parent->type) {
        case NODE_DOCUMENT:
            children = &parent->data.document.children;
            break;
        case NODE_ELEMENT:
            children = &parent->data.element.children;
            break;
        case NODE_LET:
        case NODE_VAR:
            children = &parent->data.binding.body;
            break;
        default:
            return;
    }

    if (*children == NULL) {
        *children = child;
    } else {
        AstNode *last = *children;
        while (last->next) {
            last = last->next;
        }
        last->next = child;
    }
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

void ast_print(AstNode *node, int indent) {
    if (!node) return;

    print_indent(indent);
    printf("%s", node_type_name(node->type));

    switch (node->type) {
        case NODE_ELEMENT:
            printf(" <%s>", node->data.element.tag);
            printf("\n");
            for (AstNode *attr = node->data.element.attrs; attr; attr = attr->next) {
                ast_print(attr, indent + 1);
            }
            for (AstNode *child = node->data.element.children; child; child = child->next) {
                ast_print(child, indent + 1);
            }
            return;

        case NODE_TEXT:
            printf(" \"%.20s%s\"", node->data.text.content,
                   node->data.text.length > 20 ? "..." : "");
            break;

        case NODE_ATTR:
            printf(" %s=", node->data.attr.name);
            if (node->data.attr.value) {
                printf("\n");
                ast_print(node->data.attr.value, indent + 1);
                return;
            }
            break;

        case NODE_LET:
        case NODE_VAR:
            printf(" %s", node->data.binding.name);
            if (node->data.binding.dynamic) printf(" [dynamic]");
            if (node->data.binding.single) printf(" [single]");
            printf("\n");
            if (node->data.binding.value) {
                print_indent(indent + 1);
                printf("value:\n");
                ast_print(node->data.binding.value, indent + 2);
            }
            if (node->data.binding.sql) {
                print_indent(indent + 1);
                printf("sql:\n");
                ast_print(node->data.binding.sql, indent + 2);
            }
            if (node->data.binding.body) {
                print_indent(indent + 1);
                printf("body:\n");
                for (AstNode *child = node->data.binding.body; child; child = child->next) {
                    ast_print(child, indent + 2);
                }
            }
            return;

        case NODE_IDENT:
            printf(" %s", node->data.ident.name);
            break;

        case NODE_LITERAL:
            switch (node->data.literal.lit_type) {
                case LIT_NUMBER:
                    printf(" %g", node->data.literal.v.number);
                    break;
                case LIT_INT:
                    printf(" %lld", (long long)node->data.literal.v.integer);
                    break;
                case LIT_STRING:
                    printf(" \"%s\"", node->data.literal.v.string.value);
                    break;
                case LIT_BOOL:
                    printf(" %s", node->data.literal.v.boolean ? "true" : "false");
                    break;
                case LIT_NULL:
                    printf(" null");
                    break;
            }
            break;

        case NODE_BINARY:
            printf(" op=%d\n", node->data.binary.op);
            ast_print(node->data.binary.left, indent + 1);
            ast_print(node->data.binary.right, indent + 1);
            return;

        case NODE_MEMBER:
            printf(" .%s\n", node->data.member.member);
            ast_print(node->data.member.object, indent + 1);
            return;

        case NODE_OUTPUT:
            printf("\n");
            ast_print(node->data.output.expr, indent + 1);
            return;

        case NODE_FOR:
            printf(" %s in\n", node->data.for_loop.item);
            ast_print(node->data.for_loop.iterable, indent + 1);
            print_indent(indent + 1);
            printf("body:\n");
            for (AstNode *child = node->data.for_loop.body; child; child = child->next) {
                ast_print(child, indent + 2);
            }
            return;

        case NODE_DEFCOMP:
            printf(" %s\n", node->data.defcomp.name);
            if (node->data.defcomp.props) {
                print_indent(indent + 1);
                printf("props:\n");
                for (AstNode *p = node->data.defcomp.props; p; p = p->next) {
                    ast_print(p, indent + 2);
                }
            }
            if (node->data.defcomp.body) {
                print_indent(indent + 1);
                printf("body:\n");
                for (AstNode *child = node->data.defcomp.body; child; child = child->next) {
                    ast_print(child, indent + 2);
                }
            }
            return;

        default:
            break;
    }

    printf("\n");
}
