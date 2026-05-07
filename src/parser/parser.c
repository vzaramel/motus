/*
 * Motus Parser Implementation
 *
 * Recursive descent parser for XML-based syntax.
 */

#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static AstNode *parse_node(Parser *p);
static AstNode *parse_element(Parser *p);
static AstNode *parse_expression(Parser *p);
static AstNode *parse_primary(Parser *p);
static AstNode *parse_sql(Parser *p);
static AstNode *parse_match(Parser *p);
static AstNode *parse_export(Parser *p);
static AstNode *parse_interface(Parser *p);
static AstNode *parse_require_auth(Parser *p);

/* ============ Token Manipulation ============ */

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = lexer_next(&p->lexer);

    if (p->current.type == TOK_ERROR) {
        p->had_error = true;
        /* Add error to list */
        if (p->errors && p->errors->count < p->errors->capacity) {
            MotError *err = &p->errors->errors[p->errors->count++];
            err->message = p->current.start;
            err->line = p->current.line;
            err->column = p->current.column;
            err->file = NULL;
        }
    }
}

static bool check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static bool match(Parser *p, TokenType type) {
    if (!check(p, type)) return false;
    advance(p);
    return true;
}

static void error_at(Parser *p, Token *token, const char *message) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error = true;

    fprintf(stderr, "[line %d, col %d] Error", token->line, token->column);
    if (token->type == TOK_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOK_ERROR) {
        fprintf(stderr, " at '%.*s'", (int)token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);

    if (p->errors && p->errors->count < p->errors->capacity) {
        MotError *err = &p->errors->errors[p->errors->count++];
        err->message = message;
        err->line = token->line;
        err->column = token->column;
        err->file = NULL;
    }
}

static void error(Parser *p, const char *message) {
    error_at(p, &p->previous, message);
}

static void error_current(Parser *p, const char *message) {
    error_at(p, &p->current, message);
}

static bool consume(Parser *p, TokenType type, const char *message) {
    if (check(p, type)) {
        advance(p);
        return true;
    }
    error_current(p, message);
    return false;
}

/* Check if current token can be used as an identifier (including keywords) */
static bool is_name_token(Parser *p) {
    TokenType t = p->current.type;
    /* Accept identifiers and all keywords as potential names */
    return t == TOK_IDENT || token_is_keyword(t);
}

/* Consume a closing tag like </tagname>, returns true if matched the expected tag */
static bool consume_closing_tag(Parser *p, const char *expected_tag) {
    if (!match(p, TOK_LT_SLASH)) return false;

    if (is_name_token(p)) {
        char *close_tag = arena_strndup(p->arena, p->current.start, p->current.length);
        advance(p);
        if (expected_tag && strcmp(close_tag, expected_tag) != 0) {
            error(p, "Mismatched closing tag");
        }
    }

    consume(p, TOK_GT, "Expected '>' after closing tag");
    return true;
}

static void synchronize(Parser *p) {
    p->panic_mode = false;

    while (p->current.type != TOK_EOF) {
        /* Stop at element boundaries */
        if (p->current.type == TOK_LT || p->current.type == TOK_LT_SLASH) {
            return;
        }
        advance(p);
    }
}

/* Get identifier text from previous token */
static char *get_identifier(Parser *p) {
    return arena_strndup(p->arena, p->previous.start, p->previous.length);
}

/* ============ Expression Parsing ============ */

typedef enum {
    PREC_NONE,
    PREC_OR,         /* or */
    PREC_AND,        /* and */
    PREC_EQUALITY,   /* eq neq */
    PREC_COMPARISON, /* lt gt lte gte */
    PREC_TERM,       /* + - */
    PREC_FACTOR,     /* * / % */
    PREC_UNARY,      /* not - */
    PREC_CALL,       /* . [] () */
    PREC_PRIMARY,
} Precedence;

static AstNode *parse_precedence(Parser *p, Precedence prec);

static OpType token_to_op(TokenType type) {
    switch (type) {
        case TOK_PLUS: return OP_ADD;
        case TOK_MINUS: return OP_SUB;
        case TOK_STAR: return OP_MUL;
        case TOK_SLASH: return OP_DIV;
        case TOK_PERCENT: return OP_MOD;
        case TOK_LT_KW: return OP_LT;
        case TOK_GT_KW: return OP_GT;
        case TOK_LTE: return OP_LTE;
        case TOK_GTE: return OP_GTE;
        case TOK_EQ_KW: return OP_EQ;
        case TOK_EQ: return OP_EQ;     /* = in SQL context */
        case TOK_NEQ: return OP_NEQ;
        case TOK_AND: return OP_AND;
        case TOK_OR: return OP_OR;
        case TOK_NOT: return OP_NOT;
        default: return OP_ADD; /* shouldn't happen */
    }
}

static Precedence get_precedence(TokenType type) {
    switch (type) {
        case TOK_OR: return PREC_OR;
        case TOK_AND: return PREC_AND;
        case TOK_EQ_KW:
        case TOK_EQ:      /* = in SQL context */
        case TOK_NEQ: return PREC_EQUALITY;
        case TOK_LT_KW:
        case TOK_GT_KW:
        case TOK_LTE:
        case TOK_GTE: return PREC_COMPARISON;
        case TOK_PLUS:
        case TOK_MINUS: return PREC_TERM;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT: return PREC_FACTOR;
        case TOK_DOT:
        case TOK_LBRACKET: return PREC_CALL;
        default: return PREC_NONE;
    }
}

static AstNode *parse_number(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Check if it's an integer or float */
    bool has_dot = false;
    for (size_t i = 0; i < p->previous.length; i++) {
        if (p->previous.start[i] == '.' || p->previous.start[i] == 'e' || p->previous.start[i] == 'E') {
            has_dot = true;
            break;
        }
    }

    if (has_dot) {
        double value = strtod(p->previous.start, NULL);
        return ast_literal_number(p->arena, value, line, col);
    } else {
        int64_t value = strtoll(p->previous.start, NULL, 10);
        return ast_literal_int(p->arena, value, line, col);
    }
}

static AstNode *parse_string(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Skip quotes */
    const char *value = p->previous.start + 1;
    size_t length = p->previous.length - 2;

    return ast_literal_string(p->arena, value, length, line, col);
}

static AstNode *parse_identifier_or_call(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;
    char *name = get_identifier(p);

    /* Check if it's followed by arguments (function call) */
    /* In Motus, function calls are space-separated: fn arg1 arg2 */
    /* But we need to be careful not to consume too much */

    /* For now, just return identifier - calls are handled differently */
    return ast_ident(p->arena, name, line, col);
}

static AstNode *parse_array(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *elements = NULL;
    AstNode **tail = &elements;

    if (!check(p, TOK_RBRACKET)) {
        do {
            AstNode *elem = parse_expression(p);
            *tail = elem;
            tail = &elem->next;
        } while (match(p, TOK_COMMA) || (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)));
    }

    consume(p, TOK_RBRACKET, "Expected ']' after array elements");
    return ast_array(p->arena, elements, line, col);
}

static AstNode *parse_object(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *pairs = NULL;
    AstNode **tail = &pairs;

    if (!check(p, TOK_RBRACE)) {
        do {
            /* Key */
            if (!match(p, TOK_IDENT) && !match(p, TOK_STRING)) {
                error_current(p, "Expected object key");
                break;
            }
            char *key = get_identifier(p);
            int key_line = p->previous.line;
            int key_col = p->previous.column;

            consume(p, TOK_COLON, "Expected ':' after object key");

            /* Value */
            AstNode *value = parse_expression(p);

            /* Create a pair as attr node (reusing) */
            AstNode *pair = ast_attr(p->arena, key, value, key_line, key_col);
            *tail = pair;
            tail = &pair->next;

        } while (match(p, TOK_COMMA));
    }

    consume(p, TOK_RBRACE, "Expected '}' after object");
    return ast_object(p->arena, pairs, line, col);
}

static AstNode *parse_grouped(Parser *p) {
    AstNode *expr = parse_expression(p);
    consume(p, TOK_RPAREN, "Expected ')' after expression");
    return expr;
}

static AstNode *parse_unary(Parser *p) {
    TokenType op_type = p->previous.type;
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *operand = parse_precedence(p, PREC_UNARY);

    OpType op = (op_type == TOK_MINUS) ? OP_NEG : OP_NOT;
    return ast_unary(p->arena, op, operand, line, col);
}

/* Parse nested function call: <fn arg> */
static AstNode *parse_angle_call(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* We just consumed '<', now expect identifier */
    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected function name after '<'");
        return NULL;
    }

    char *fn_name = get_identifier(p);
    AstNode *args = NULL;
    AstNode **tail = &args;

    /* Parse arguments until '>' */
    while (!check(p, TOK_GT) && !check(p, TOK_EOF)) {
        AstNode *arg = parse_primary(p);
        if (!arg) break;
        *tail = arg;
        tail = &arg->next;
    }

    consume(p, TOK_GT, "Expected '>' after function call");

    return ast_call(p->arena, fn_name, args, line, col);
}

/* Parse ternary: if cond then a else b */
static AstNode *parse_ternary_expr(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *condition = parse_expression(p);

    if (!consume(p, TOK_THEN, "Expected 'then' after condition")) {
        return condition;
    }

    AstNode *then_expr = parse_expression(p);

    if (!consume(p, TOK_ELSE, "Expected 'else' in ternary expression")) {
        return then_expr;
    }

    AstNode *else_expr = parse_expression(p);

    return ast_ternary(p->arena, condition, then_expr, else_expr, line, col);
}

static AstNode *parse_primary(Parser *p) {
    if (match(p, TOK_NUMBER)) {
        return parse_number(p);
    }

    if (match(p, TOK_STRING)) {
        return parse_string(p);
    }

    if (match(p, TOK_TRUE)) {
        return ast_literal_bool(p->arena, true, p->previous.line, p->previous.column);
    }

    if (match(p, TOK_FALSE)) {
        return ast_literal_bool(p->arena, false, p->previous.line, p->previous.column);
    }

    if (match(p, TOK_NULL)) {
        return ast_literal_null(p->arena, p->previous.line, p->previous.column);
    }

    if (match(p, TOK_COLON)) {
        int line = p->previous.line;
        int col = p->previous.column;
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected SQL parameter name after ':'");
            return NULL;
        }
        return ast_sql_param(p->arena, get_identifier(p), line, col);
    }

    if (match(p, TOK_IDENT)) {
        return parse_identifier_or_call(p);
    }

    if (match(p, TOK_LBRACKET)) {
        return parse_array(p);
    }

    if (match(p, TOK_LBRACE)) {
        return parse_object(p);
    }

    if (match(p, TOK_LPAREN)) {
        return parse_grouped(p);
    }

    if (match(p, TOK_LT)) {
        return parse_angle_call(p);
    }

    if (match(p, TOK_MINUS) || match(p, TOK_NOT)) {
        return parse_unary(p);
    }

    if (match(p, TOK_IF)) {
        return parse_ternary_expr(p);
    }

    error_current(p, "Expected expression");
    return NULL;
}

static AstNode *parse_postfix(Parser *p, AstNode *left) {
    for (;;) {
        if (match(p, TOK_DOT)) {
            /* Member access */
            if (!match(p, TOK_IDENT)) {
                error_current(p, "Expected identifier after '.'");
                return left;
            }
            char *member = get_identifier(p);
            left = ast_member(p->arena, left, member, p->previous.line, p->previous.column);
        } else if (match(p, TOK_LBRACKET)) {
            /* Index access */
            AstNode *index = parse_expression(p);
            consume(p, TOK_RBRACKET, "Expected ']' after index");
            left = ast_index(p->arena, left, index, p->previous.line, p->previous.column);
        } else if (match(p, TOK_DOTDOT)) {
            /* Range: left..right */
            AstNode *end = parse_precedence(p, PREC_TERM);
            left = ast_range(p->arena, left, end, left->line, left->column);
        } else {
            break;
        }
    }
    return left;
}

static AstNode *parse_precedence(Parser *p, Precedence prec) {
    AstNode *left = parse_primary(p);
    if (!left) return NULL;

    left = parse_postfix(p, left);

    while (prec <= get_precedence(p->current.type)) {
        TokenType op_type = p->current.type;
        int line = p->current.line;
        int col = p->current.column;

        advance(p);

        AstNode *right = parse_precedence(p, get_precedence(op_type) + 1);
        if (!right) return left;

        left = ast_binary(p->arena, token_to_op(op_type), left, right, line, col);
    }

    return left;
}

static AstNode *parse_pipe_expression(Parser *p) {
    AstNode *expr = parse_precedence(p, PREC_OR);
    if (!expr) return NULL;

    if (!check(p, TOK_PIPE)) {
        return expr;
    }

    /* Parse pipe chain */
    int line = expr->line;
    int col = expr->column;
    AstNode *stages = NULL;
    AstNode **tail = &stages;

    while (match(p, TOK_PIPE)) {
        /* Expect function name */
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected function name after '|'");
            break;
        }

        char *fn_name = get_identifier(p);
        int fn_line = p->previous.line;
        int fn_col = p->previous.column;

        /* Parse optional arguments */
        AstNode *args = NULL;
        AstNode **arg_tail = &args;

        while (!check(p, TOK_PIPE) && !check(p, TOK_GT) &&
               !check(p, TOK_EOF) && !check(p, TOK_LT_SLASH)) {
            /* Stop at things that end the expression */
            if (check(p, TOK_IDENT) || check(p, TOK_STRING) ||
                check(p, TOK_NUMBER) || check(p, TOK_LBRACKET) ||
                check(p, TOK_LBRACE) || check(p, TOK_LT)) {
                AstNode *arg = parse_primary(p);
                if (arg) {
                    arg = parse_postfix(p, arg);
                    *arg_tail = arg;
                    arg_tail = &arg->next;
                }
            } else {
                break;
            }
        }

        AstNode *call = ast_call(p->arena, fn_name, args, fn_line, fn_col);
        *tail = call;
        tail = &call->next;
    }

    return ast_pipe(p->arena, expr, stages, line, col);
}

static AstNode *parse_expression(Parser *p) {
    return parse_pipe_expression(p);
}

/* ============ SQL Parsing ============ */

static AstNode *parse_sql_column_list(Parser *p) {
    AstNode *columns = NULL;
    AstNode **tail = &columns;

    do {
        int line = p->current.line;
        int col = p->current.column;

        /* Check for * */
        if (match(p, TOK_STAR)) {
            AstNode *star = ast_ident(p->arena, "*", line, col);
            AstNode *column = ast_node_new(p->arena, NODE_SQL_COLUMN, line, col);
            column->data.sql_column.expr = star;
            column->data.sql_column.alias = NULL;
            *tail = column;
            tail = &column->next;
            continue;
        }

        /* Parse column expression */
        AstNode *expr = parse_expression(p);
        if (!expr) break;

        /* Check for alias */
        char *alias = NULL;
        if (match(p, TOK_AS)) {
            if (!match(p, TOK_IDENT)) {
                error_current(p, "Expected alias name after 'as'");
            } else {
                alias = get_identifier(p);
            }
        }

        AstNode *column = ast_node_new(p->arena, NODE_SQL_COLUMN, line, col);
        column->data.sql_column.expr = expr;
        column->data.sql_column.alias = alias;

        *tail = column;
        tail = &column->next;

    } while (match(p, TOK_COMMA));

    return columns;
}

static AstNode *parse_sql_from(Parser *p) {
    int line = p->current.line;
    int col = p->current.column;

    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected table name after 'from'");
        return NULL;
    }

    char *table = get_identifier(p);
    char *alias = NULL;

    if (match(p, TOK_AS) || check(p, TOK_IDENT)) {
        if (p->previous.type != TOK_AS) {
            /* Implicit alias */
        }
        if (match(p, TOK_IDENT)) {
            alias = get_identifier(p);
        }
    }

    AstNode *from = ast_node_new(p->arena, NODE_SQL_FROM, line, col);
    from->data.sql_table.table = table;
    from->data.sql_table.alias = alias;
    from->data.sql_table.on_condition = NULL;
    from->data.sql_table.join_type = 0;

    return from;
}

static AstNode *parse_sql(Parser *p) {
    int line = p->previous.line;  /* 'select' was already consumed */
    int col = p->previous.column;

    /* SELECT columns */
    AstNode *columns = parse_sql_column_list(p);

    /* FROM table */
    AstNode *from = NULL;
    if (match(p, TOK_FROM)) {
        from = parse_sql_from(p);
    }

    /* JOIN clauses (TODO: implement properly) */
    AstNode *joins = NULL;

    /* WHERE condition */
    AstNode *where = NULL;
    if (match(p, TOK_WHERE)) {
        where = parse_expression(p);
    }

    /* ORDER BY */
    AstNode *order = NULL;
    if (match(p, TOK_ORDER)) {
        consume(p, TOK_BY, "Expected 'by' after 'order'");

        AstNode **order_tail = &order;
        do {
            int ord_line = p->current.line;
            int ord_col = p->current.column;

            AstNode *expr = parse_expression(p);
            bool desc = false;

            if (match(p, TOK_DESC)) {
                desc = true;
            } else {
                match(p, TOK_ASC);  /* optional */
            }

            AstNode *ord = ast_node_new(p->arena, NODE_SQL_ORDER, ord_line, ord_col);
            ord->data.sql_order.expr = expr;
            ord->data.sql_order.descending = desc;

            *order_tail = ord;
            order_tail = &ord->next;

        } while (match(p, TOK_COMMA));
    }

    /* LIMIT */
    AstNode *limit = NULL;
    if (match(p, TOK_LIMIT)) {
        limit = parse_expression(p);
    }

    /* OFFSET */
    AstNode *offset = NULL;
    if (match(p, TOK_OFFSET)) {
        offset = parse_expression(p);
    }

    return ast_sql(p->arena, columns, from, joins, where, order, limit, offset, line, col);
}

/* ============ Element Parsing ============ */

static AstNode *parse_let_or_var(Parser *p, bool is_var) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Parse bindings: name value name2 value2 ... */
    /* Can also have: dynamic, single, select ... */

    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected variable name");
        return NULL;
    }

    char *name = get_identifier(p);
    bool dynamic = false;
    bool single = false;
    AstNode *value = NULL;
    AstNode *sql = NULL;

    /* Check for = */
    match(p, TOK_EQ);

    /* Check for modifiers and value */
    if (match(p, TOK_DYNAMIC)) {
        dynamic = true;
        match(p, TOK_EQ);  /* optional = after dynamic */
    }

    if (match(p, TOK_SINGLE)) {
        single = true;
    }

    /* Check for SQL query */
    if (match(p, TOK_SELECT)) {
        sql = parse_sql(p);
    } else if (!check(p, TOK_GT) && !check(p, TOK_SLASH_GT)) {
        /* Parse value expression */
        lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
        value = parse_expression(p);
        lexer_set_mode(&p->lexer, LEX_MODE_XML);
    }

    /* Check for self-closing or children */
    AstNode *body = NULL;

    if (match(p, TOK_SLASH_GT)) {
        /* Self-closing */
    } else if (match(p, TOK_GT)) {
        /* Has children */
        AstNode **tail = &body;
        while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
            AstNode *child = parse_node(p);
            if (child) {
                *tail = child;
                tail = &child->next;
            }
        }

        /* Consume closing tag */
        consume_closing_tag(p, is_var ? "var" : "let");
    }

    if (is_var) {
        return ast_var(p->arena, name, value, sql, dynamic, single, line, col);
    } else {
        return ast_let(p->arena, name, value, sql, body, dynamic, single, line, col);
    }
}

static AstNode *parse_set(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Parse target (identifier or member access like x.y) */
    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);

    /* First get identifier */
    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected variable name after 'set'");
        lexer_set_mode(&p->lexer, LEX_MODE_XML);
        return NULL;
    }

    AstNode *target = ast_ident(p->arena, get_identifier(p), p->previous.line, p->previous.column);

    /* Check for member access (x.y.z) */
    while (match(p, TOK_DOT)) {
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected field name after '.'");
            lexer_set_mode(&p->lexer, LEX_MODE_XML);
            return NULL;
        }
        target = ast_member(p->arena, target, get_identifier(p), p->previous.line, p->previous.column);
    }

    /* Parse value expression */
    AstNode *value = parse_expression(p);
    lexer_set_mode(&p->lexer, LEX_MODE_XML);

    /* Accept either > or /> (self-closing) */
    if (!match(p, TOK_GT) && !match(p, TOK_SLASH_GT)) {
        error_current(p, "Expected '>' or '/>' after set value");
    }

    return ast_set(p->arena, target, value, line, col);
}

static AstNode *parse_output(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
    AstNode *expr = parse_expression(p);
    lexer_set_mode(&p->lexer, LEX_MODE_XML);

    consume(p, TOK_GT, "Expected '>' after output expression");

    return ast_output(p->arena, expr, line, col);
}

static AstNode *parse_if(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Parse condition */
    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
    AstNode *condition = parse_expression(p);
    lexer_set_mode(&p->lexer, LEX_MODE_XML);

    consume(p, TOK_GT, "Expected '>' after if condition");

    /* Parse then body */
    AstNode *then_body = NULL;
    AstNode **tail = &then_body;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        /* Check for elsif or else (they appear as elements) */
        if (check(p, TOK_LT)) {
            Token peek = lexer_peek(&p->lexer);
            if (peek.type == TOK_ELSIF || peek.type == TOK_ELSE) {
                break;
            }
        }

        AstNode *child = parse_node(p);
        if (child) {
            /* elsif and else break out of then body */
            if (child->type == NODE_ELSIF || child->type == NODE_ELSE) {
                /* This is the else branch */
                return ast_if(p->arena, condition, then_body, child, line, col);
            }
            *tail = child;
            tail = &child->next;
        }
    }

    /* Parse else branch if present */
    AstNode *else_branch = NULL;

    if (match(p, TOK_LT)) {
        if (match(p, TOK_ELSIF)) {
            else_branch = parse_if(p);  /* Reuse if parsing */
            else_branch->type = NODE_ELSIF;
        } else if (match(p, TOK_ELSE)) {
            consume(p, TOK_GT, "Expected '>' after else");

            AstNode *else_body = NULL;
            AstNode **else_tail = &else_body;

            while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
                AstNode *child = parse_node(p);
                if (child) {
                    *else_tail = child;
                    else_tail = &child->next;
                }
            }

            else_branch = ast_else(p->arena, else_body, p->previous.line, p->previous.column);
        }
    }

    /* Consume </if> */
    if (match(p, TOK_LT_SLASH)) {
        match(p, TOK_IF);
        consume(p, TOK_GT, "Expected '>' after </if>");
    }

    return ast_if(p->arena, condition, then_body, else_branch, line, col);
}

static AstNode *parse_for(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Parse loop variable(s) */
    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected loop variable name");
        return NULL;
    }

    char *item = get_identifier(p);
    char *index = NULL;

    /* Check for index variable: for item, index in ... */
    if (match(p, TOK_COMMA)) {
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected index variable name");
            return NULL;
        }
        index = get_identifier(p);
    }

    /* Expect 'in' */
    if (!consume(p, TOK_IN, "Expected 'in' after loop variable")) {
        return NULL;
    }

    /* Parse iterable (could be expression or inline SQL) */
    AstNode *iterable;
    if (match(p, TOK_SELECT)) {
        iterable = parse_sql(p);
    } else {
        lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
        iterable = parse_expression(p);
        lexer_set_mode(&p->lexer, LEX_MODE_XML);
    }

    consume(p, TOK_GT, "Expected '>' after for header");

    /* Parse body */
    AstNode *body = NULL;
    AstNode **tail = &body;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        AstNode *child = parse_node(p);
        if (child) {
            *tail = child;
            tail = &child->next;
        }
    }

    /* Consume </for> */
    if (match(p, TOK_LT_SLASH)) {
        match(p, TOK_FOR);
        consume(p, TOK_GT, "Expected '>' after </for>");
    }

    return ast_for(p->arena, item, index, iterable, body, line, col);
}

static AstNode *parse_match_case(Parser *p, bool is_default) {
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *pattern = NULL;
    if (!is_default) {
        lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
        pattern = parse_expression(p);
        lexer_set_mode(&p->lexer, LEX_MODE_XML);
    }

    AstNode *body = NULL;
    if (match(p, TOK_SLASH_GT)) {
        return ast_case(p->arena, pattern, body, is_default, line, col);
    }

    consume(p, TOK_GT, is_default ? "Expected '>' after default" : "Expected '>' after case pattern");

    AstNode **tail = &body;
    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        AstNode *child = parse_node(p);
        if (child) {
            *tail = child;
            tail = &child->next;
        }
    }

    if (match(p, TOK_LT_SLASH)) {
        if (is_default) {
            if (!match(p, TOK_DEFAULT)) {
                error_current(p, "Expected </default>");
            }
        } else {
            if (!match(p, TOK_CASE)) {
                error_current(p, "Expected </case>");
            }
        }
        consume(p, TOK_GT, "Expected '>' after case/default closing tag");
    } else {
        error_current(p, "Expected closing tag for case/default");
    }

    return ast_case(p->arena, pattern, body, is_default, line, col);
}

static AstNode *parse_match(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
    AstNode *value = parse_expression(p);
    lexer_set_mode(&p->lexer, LEX_MODE_XML);

    consume(p, TOK_GT, "Expected '>' after match value");

    AstNode *cases = NULL;
    AstNode **tail = &cases;
    bool has_default = false;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        if (match(p, TOK_TEXT)) {
            bool all_whitespace = true;
            for (size_t i = 0; i < p->previous.length; i++) {
                if (!is_whitespace(p->previous.start[i])) {
                    all_whitespace = false;
                    break;
                }
            }
            if (all_whitespace) {
                continue;
            }
            error(p, "Only <case> and <default> are allowed inside <match>");
            continue;
        }

        if (!match(p, TOK_LT)) {
            error_current(p, "Expected <case> or <default> inside <match>");
            advance(p);
            continue;
        }

        if (match(p, TOK_CASE)) {
            AstNode *case_node = parse_match_case(p, false);
            if (case_node) {
                *tail = case_node;
                tail = &case_node->next;
            }
            continue;
        }

        if (match(p, TOK_DEFAULT)) {
            if (has_default) {
                error_current(p, "Only one <default> is allowed in <match>");
            }
            has_default = true;
            AstNode *case_node = parse_match_case(p, true);
            if (case_node) {
                *tail = case_node;
                tail = &case_node->next;
            }
            continue;
        }

        error_current(p, "Expected <case> or <default> inside <match>");
        while (!check(p, TOK_GT) && !check(p, TOK_SLASH_GT) && !check(p, TOK_EOF)) {
            advance(p);
        }
        match(p, TOK_GT);
        match(p, TOK_SLASH_GT);
    }

    if (match(p, TOK_LT_SLASH)) {
        if (!match(p, TOK_MATCH)) {
            error_current(p, "Expected </match>");
        }
        consume(p, TOK_GT, "Expected '>' after </match>");
    } else {
        error_current(p, "Expected </match>");
    }

    return ast_match(p->arena, value, cases, line, col);
}

static AstNode *parse_import(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    AstNode *names = NULL;
    AstNode **tail = &names;
    char *as_name = NULL;
    bool is_external = false;
    bool is_dynamic = false;

    /* Check for * as Name */
    if (match(p, TOK_STAR)) {
        if (!consume(p, TOK_AS, "Expected 'as' after '*'")) {
            return NULL;
        }
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected name after 'as'");
            return NULL;
        }
        as_name = get_identifier(p);
    } else {
        /* Parse name list */
        do {
            if (!match(p, TOK_IDENT)) {
                error_current(p, "Expected import name");
                break;
            }
            AstNode *name = ast_ident(p->arena, get_identifier(p),
                                      p->previous.line, p->previous.column);
            *tail = name;
            tail = &name->next;
        } while (match(p, TOK_COMMA));
    }

    /* from "path" */
    if (!consume(p, TOK_FROM, "Expected 'from' in import")) {
        return NULL;
    }

    if (!match(p, TOK_STRING)) {
        error_current(p, "Expected path string after 'from'");
        return NULL;
    }

    /* Extract path (skip quotes) */
    char *from_path = arena_strndup(p->arena, p->previous.start + 1, p->previous.length - 2);

    /* Check for 'external' or 'dynamic' modifiers */
    while (check(p, TOK_EXTERNAL) || check(p, TOK_DYNAMIC)) {
        if (match(p, TOK_EXTERNAL)) {
            is_external = true;
        }
        if (match(p, TOK_DYNAMIC)) {
            is_dynamic = true;
        }
    }

    consume(p, TOK_GT, "Expected '>' after import");

    return ast_import(p->arena, names, from_path, is_external, is_dynamic, as_name, line, col);
}

static AstNode *parse_export(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    bool is_default = match(p, TOK_DEFAULT);
    AstNode *names = NULL;
    AstNode **tail = &names;

    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected exported symbol name");
    } else {
        AstNode *name = ast_ident(p->arena, get_identifier(p),
                                  p->previous.line, p->previous.column);
        *tail = name;
        tail = &name->next;
    }

    while (match(p, TOK_COMMA)) {
        if (!match(p, TOK_IDENT)) {
            error_current(p, "Expected exported symbol name");
            break;
        }
        AstNode *name = ast_ident(p->arena, get_identifier(p),
                                  p->previous.line, p->previous.column);
        *tail = name;
        tail = &name->next;
    }

    if (match(p, TOK_SLASH_GT)) {
        return ast_export(p->arena, names, is_default, line, col);
    }

    consume(p, TOK_GT, "Expected '>' after export");

    if (match(p, TOK_LT_SLASH)) {
        if (!match(p, TOK_EXPORT)) {
            error_current(p, "Expected </export>");
        }
        consume(p, TOK_GT, "Expected '>' after </export>");
    }

    return ast_export(p->arena, names, is_default, line, col);
}

static AstNode *parse_interface(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected interface name");
        return NULL;
    }

    char *name = get_identifier(p);
    AstNode *props = NULL;
    AstNode **props_tail = &props;

    if (match(p, TOK_PIPE)) {
        while (!check(p, TOK_PIPE) && !check(p, TOK_EOF)) {
            if (!match(p, TOK_IDENT)) {
                if (check(p, TOK_PIPE)) break;
                error_current(p, "Expected interface property name");
                break;
            }

            char *prop_name = get_identifier(p);
            int prop_line = p->previous.line;
            int prop_col = p->previous.column;
            char *type_name = NULL;
            AstNode *default_val = NULL;
            bool infer_type = false;

            if (match(p, TOK_COLON_EQ)) {
                infer_type = true;
                lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
                default_val = parse_precedence(p, PREC_OR);
                lexer_set_mode(&p->lexer, LEX_MODE_XML);
            } else if (match(p, TOK_COLON)) {
                if (!match(p, TOK_IDENT)) {
                    error_current(p, "Expected type name");
                } else {
                    type_name = get_identifier(p);
                }

                if (match(p, TOK_EQ)) {
                    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
                    default_val = parse_precedence(p, PREC_OR);
                    lexer_set_mode(&p->lexer, LEX_MODE_XML);
                }
            }

            bool required = (default_val == NULL && !infer_type);
            AstNode *prop = ast_prop_def(p->arena, prop_name, type_name, default_val,
                                         required, infer_type, prop_line, prop_col);
            *props_tail = prop;
            props_tail = &prop->next;

            match(p, TOK_COMMA);
        }

        consume(p, TOK_PIPE, "Expected '|' after interface properties");
    }

    AstNode *iface = ast_node_new(p->arena, NODE_INTERFACE, line, col);
    iface->data.interface.name = arena_strdup(p->arena, name);
    iface->data.interface.props = props;

    if (match(p, TOK_SLASH_GT)) {
        return iface;
    }

    consume(p, TOK_GT, "Expected '>' after interface declaration");

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        advance(p);
    }

    if (match(p, TOK_LT_SLASH)) {
        if (!match(p, TOK_INTERFACE)) {
            error_current(p, "Expected </interface>");
        }
        consume(p, TOK_GT, "Expected '>' after </interface>");
    }

    return iface;
}

/* Parse <require-auth role="admin" /> */
static AstNode *parse_require_auth(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;
    char *role = NULL;

    /* Optional role attribute */
    while (!check(p, TOK_SLASH_GT) && !check(p, TOK_GT) && !check(p, TOK_EOF)) {
        if (match(p, TOK_IDENT)) {
            char *attr_name = get_identifier(p);
            if (strcmp(attr_name, "role") == 0 && match(p, TOK_EQ) && match(p, TOK_STRING)) {
                /* Extract string content without quotes */
                role = arena_strndup(p->arena, p->previous.start + 1, p->previous.length - 2);
            }
        } else {
            advance(p);  /* Skip unexpected tokens */
        }
    }

    /* Accept either /> or > */
    if (!match(p, TOK_SLASH_GT)) {
        consume(p, TOK_GT, "Expected '>' or '/>' after require-auth");
    }

    return ast_require_auth(p->arena, role, line, col);
}

static AstNode *parse_defcomp(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Component name */
    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected component name");
        return NULL;
    }

    char *name = get_identifier(p);

    /* Props: | prop1 : type, prop2 : type = default | */
    AstNode *props = NULL;
    AstNode **props_tail = &props;

    if (match(p, TOK_PIPE)) {
        while (!check(p, TOK_PIPE) && !check(p, TOK_EOF)) {
            if (!match(p, TOK_IDENT)) {
                if (check(p, TOK_PIPE)) break;
                error_current(p, "Expected prop name");
                break;
            }

            char *prop_name = get_identifier(p);
            int prop_line = p->previous.line;
            int prop_col = p->previous.column;
            char *type_name = NULL;
            AstNode *default_val = NULL;
            bool infer_type = false;

            /* Check for : type or := default */
            if (match(p, TOK_COLON_EQ)) {
                /* Type inference from default */
                infer_type = true;
                lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
                /* Use parse_precedence to avoid consuming closing | as pipe */
                default_val = parse_precedence(p, PREC_OR);
                lexer_set_mode(&p->lexer, LEX_MODE_XML);
            } else if (match(p, TOK_COLON)) {
                /* Explicit type */
                if (!match(p, TOK_IDENT)) {
                    error_current(p, "Expected type name");
                } else {
                    type_name = get_identifier(p);
                }

                /* Optional default */
                if (match(p, TOK_EQ)) {
                    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
                    /* Use parse_precedence to avoid consuming closing | as pipe */
                    default_val = parse_precedence(p, PREC_OR);
                    lexer_set_mode(&p->lexer, LEX_MODE_XML);
                }
            }

            bool required = (default_val == NULL && !infer_type);
            AstNode *prop = ast_prop_def(p->arena, prop_name, type_name, default_val,
                                         required, infer_type, prop_line, prop_col);
            *props_tail = prop;
            props_tail = &prop->next;

            match(p, TOK_COMMA);  /* Optional comma */
        }

        consume(p, TOK_PIPE, "Expected '|' after props");
    }

    /* Slots: slots name1 : interface1, name2 : interface2 */
    AstNode *slots = NULL;
    AstNode **slots_tail = &slots;

    if (match(p, TOK_SLOTS)) {
        while (match(p, TOK_IDENT)) {
            char *slot_name = get_identifier(p);
            int slot_line = p->previous.line;
            int slot_col = p->previous.column;
            char *interface = NULL;

            if (match(p, TOK_COLON)) {
                if (match(p, TOK_IDENT)) {
                    interface = get_identifier(p);
                }
            }

            AstNode *slot = ast_slot_def(p->arena, slot_name, interface, slot_line, slot_col);
            *slots_tail = slot;
            slots_tail = &slot->next;

            match(p, TOK_COMMA);  /* Optional comma */
        }
    }

    consume(p, TOK_GT, "Expected '>' after defcomp header");

    /* Parse body */
    AstNode *body = NULL;
    AstNode **tail = &body;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        AstNode *child = parse_node(p);
        if (child) {
            *tail = child;
            tail = &child->next;
        }
    }

    /* Consume </defcomp> */
    if (match(p, TOK_LT_SLASH)) {
        match(p, TOK_DEFCOMP);
        consume(p, TOK_GT, "Expected '>' after </defcomp>");
    }

    return ast_defcomp(p->arena, name, props, slots, body, line, col);
}

static AstNode *parse_macro(Parser *p) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Macro name */
    if (!match(p, TOK_IDENT)) {
        error_current(p, "Expected macro name");
        return NULL;
    }

    char *name = get_identifier(p);

    /* Optional params: |param1, param2| */
    AstNode *params = NULL;
    AstNode **params_tail = &params;

    if (match(p, TOK_PIPE)) {
        while (!check(p, TOK_PIPE) && !check(p, TOK_EOF)) {
            if (!match(p, TOK_IDENT)) {
                if (check(p, TOK_PIPE)) break;
                error_current(p, "Expected parameter name");
                break;
            }

            AstNode *param = ast_ident(p->arena, get_identifier(p),
                                       p->previous.line, p->previous.column);
            *params_tail = param;
            params_tail = &param->next;

            match(p, TOK_COMMA);
        }

        consume(p, TOK_PIPE, "Expected '|' after macro params");
    }

    consume(p, TOK_GT, "Expected '>' after macro header");

    /* Parse body */
    AstNode *body = NULL;
    AstNode **tail = &body;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        AstNode *child = parse_node(p);
        if (child) {
            *tail = child;
            tail = &child->next;
        }
    }

    /* Consume </macro> */
    if (match(p, TOK_LT_SLASH)) {
        match(p, TOK_MACRO);
        consume(p, TOK_GT, "Expected '>' after </macro>");
    }

    return ast_macro(p->arena, name, params, body, line, col);
}

static AstNode *parse_html_element(Parser *p, const char *tag) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Parse attributes */
    AstNode *attrs = NULL;
    AstNode **attrs_tail = &attrs;

    while (check(p, TOK_IDENT) || token_is_keyword(p->current.type)) {
        advance(p);
        char *attr_name;
        if (p->previous.type == TOK_IDENT) {
            attr_name = get_identifier(p);
        } else {
            attr_name = arena_strndup(p->arena, p->previous.start, p->previous.length);
        }
        int attr_line = p->previous.line;
        int attr_col = p->previous.column;
        AstNode *attr_value = NULL;

        if (match(p, TOK_EQ)) {
            if (match(p, TOK_STRING)) {
                attr_value = parse_string(p);
            } else {
                /* Expression value */
                lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
                attr_value = parse_expression(p);
                lexer_set_mode(&p->lexer, LEX_MODE_XML);
            }
        }

        AstNode *attr = ast_attr(p->arena, attr_name, attr_value, attr_line, attr_col);
        *attrs_tail = attr;
        attrs_tail = &attr->next;
    }

    /* Self-closing? */
    if (match(p, TOK_SLASH_GT)) {
        return ast_element(p->arena, tag, attrs, NULL, true, line, col);
    }

    if (!consume(p, TOK_GT, "Expected '>' or '/>'")) {
        return ast_element(p->arena, tag, attrs, NULL, true, line, col);
    }

    /* Parse children */
    AstNode *children = NULL;
    AstNode **tail = &children;

    while (!check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        AstNode *child = parse_node(p);
        if (child) {
            *tail = child;
            tail = &child->next;
        }
    }

    /* Closing tag */
    if (match(p, TOK_LT_SLASH)) {
        if (match(p, TOK_IDENT)) {
            char *close_tag = get_identifier(p);
            if (strcmp(close_tag, tag) != 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Expected </%s>, got </%s>", tag, close_tag);
                error(p, msg);
            }
        }
        consume(p, TOK_GT, "Expected '>' after closing tag");
    }

    return ast_element(p->arena, tag, attrs, children, false, line, col);
}

/* Parse <style> or <script> element with raw content */
static AstNode *parse_style_or_script(Parser *p, bool is_style) {
    int line = p->previous.line;
    int col = p->previous.column;

    /* Skip any attributes until we hit '>' */
    while (!check(p, TOK_GT) && !check(p, TOK_EOF)) {
        advance(p);
    }

    /* We're now at the '>' token. Record position right after it. */
    size_t content_start = p->current.start + p->current.length - p->lexer.source;

    consume(p, TOK_GT, is_style ? "Expected '>' after <style>" : "Expected '>' after <script>");

    /* Now scan raw content from content_start until we hit </style> or </script> */
    const char *tag_name = is_style ? "style" : "script";
    size_t tag_len = strlen(tag_name);
    const char *src = p->lexer.source;
    size_t src_len = p->lexer.source_len;

    /* Find the closing tag */
    size_t pos = content_start;
    while (pos < src_len) {
        /* Check for </style> or </script> */
        if (src[pos] == '<' && pos + 1 < src_len && src[pos + 1] == '/') {
            /* Check if it matches our tag */
            size_t check_pos = pos + 2;
            bool matches = true;
            for (size_t i = 0; i < tag_len && check_pos + i < src_len; i++) {
                if (src[check_pos + i] != tag_name[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches && check_pos + tag_len < src_len &&
                (src[check_pos + tag_len] == '>' || src[check_pos + tag_len] == ' ')) {
                break;  /* Found closing tag */
            }
        }
        /* Track newlines for line counting */
        if (src[pos] == '\n') {
            p->lexer.line++;
            p->lexer.column = 1;
        } else {
            p->lexer.column++;
        }
        pos++;
    }

    size_t content_len = pos - content_start;
    const char *content = src + content_start;

    /* Advance lexer past the content to the closing tag */
    p->lexer.pos = pos;

    /* Now consume the closing tag */
    advance(p);  /* Get next token, should be '</' */
    if (!match(p, TOK_LT_SLASH)) {
        error_current(p, is_style ? "Expected </style>" : "Expected </script>");
    }
    if (!match(p, is_style ? TOK_STYLE : TOK_SCRIPT)) {
        error_current(p, is_style ? "Expected </style>" : "Expected </script>");
    }
    consume(p, TOK_GT, is_style ? "Expected '>' after </style>" : "Expected '>' after </script>");

    if (is_style) {
        return ast_style(p->arena, content, content_len, line, col);
    } else {
        return ast_script(p->arena, content, content_len, line, col);
    }
}

/* Check if current token can be used as a tag name (identifier or keyword) */
static bool is_tag_name(Parser *p) {
    TokenType t = p->current.type;
    return t == TOK_IDENT ||
           t == TOK_LET || t == TOK_VAR || t == TOK_SET ||
           t == TOK_IF || t == TOK_ELSIF || t == TOK_ELSE ||
           t == TOK_FOR || t == TOK_MATCH || t == TOK_CASE ||
           t == TOK_DEFAULT || t == TOK_DEFCOMP || t == TOK_MACRO ||
           t == TOK_OUTPUT || t == TOK_IMPORT || t == TOK_EXPORT ||
           t == TOK_CHILDREN || t == TOK_INTERFACE ||
           t == TOK_STYLE || t == TOK_SCRIPT || t == TOK_REQUIRE_AUTH;
}

static AstNode *parse_element(Parser *p) {
    /* We just consumed '<', now get the tag name */
    if (!is_tag_name(p)) {
        error_current(p, "Expected tag name after '<'");
        return NULL;
    }

    TokenType tag_type = p->current.type;
    char *tag = arena_strndup(p->arena, p->current.start, p->current.length);
    advance(p);

    /* Check if it's a Motus tag */
    if (tag_type == TOK_LET) {
        return parse_let_or_var(p, false);
    }
    if (tag_type == TOK_VAR) {
        return parse_let_or_var(p, true);
    }
    if (tag_type == TOK_SET) {
        return parse_set(p);
    }
    if (tag_type == TOK_OUTPUT) {
        return parse_output(p);
    }
    if (tag_type == TOK_IF) {
        return parse_if(p);
    }
    if (tag_type == TOK_FOR) {
        return parse_for(p);
    }
    if (tag_type == TOK_MATCH) {
        return parse_match(p);
    }
    if (tag_type == TOK_IMPORT) {
        return parse_import(p);
    }
    if (tag_type == TOK_EXPORT) {
        return parse_export(p);
    }
    if (tag_type == TOK_INTERFACE) {
        return parse_interface(p);
    }
    if (tag_type == TOK_DEFCOMP) {
        return parse_defcomp(p);
    }
    if (tag_type == TOK_MACRO) {
        return parse_macro(p);
    }
    if (tag_type == TOK_CHILDREN) {
        consume(p, TOK_GT, "Expected '>' after children");
        return ast_children(p->arena, p->previous.line, p->previous.column);
    }
    if (tag_type == TOK_STYLE) {
        return parse_style_or_script(p, true);
    }
    if (tag_type == TOK_SCRIPT) {
        return parse_style_or_script(p, false);
    }
    if (tag_type == TOK_REQUIRE_AUTH) {
        return parse_require_auth(p);
    }

    /* Regular HTML element */
    return parse_html_element(p, tag);
}

static AstNode *parse_text(Parser *p) {
    int line = p->current.line;
    int col = p->current.column;

    const char *start = p->lexer.source + p->lexer.pos;
    size_t length = 0;

    /* Consume until we hit '<' or EOF */
    while (!check(p, TOK_LT) && !check(p, TOK_LT_SLASH) && !check(p, TOK_EOF)) {
        advance(p);
    }

    /* Calculate how much text we consumed */
    length = (p->lexer.source + p->lexer.pos) - start;

    if (length == 0) return NULL;

    return ast_text(p->arena, start, length, line, col);
}

static AstNode *parse_node(Parser *p) {
    /* Skip whitespace-only text for cleaner AST */
    /* But preserve text that has non-whitespace */

    if (match(p, TOK_COMMENT)) {
        return ast_comment(p->arena, p->previous.start, p->previous.length,
                          p->previous.line, p->previous.column);
    }

    if (match(p, TOK_DOCTYPE)) {
        AstNode *node = ast_node_new(p->arena, NODE_DOCTYPE,
                                     p->previous.line, p->previous.column);
        node->data.text.content = arena_strndup(p->arena, p->previous.start, p->previous.length);
        node->data.text.length = p->previous.length;
        return node;
    }

    if (match(p, TOK_TEXT)) {
        /* Check if it's just whitespace */
        bool all_whitespace = true;
        for (size_t i = 0; i < p->previous.length; i++) {
            if (!is_whitespace(p->previous.start[i])) {
                all_whitespace = false;
                break;
            }
        }
        if (all_whitespace) {
            return NULL;  /* Skip whitespace-only text */
        }
        return ast_text(p->arena, p->previous.start, p->previous.length,
                       p->previous.line, p->previous.column);
    }

    if (match(p, TOK_LT)) {
        return parse_element(p);
    }

    if (check(p, TOK_EOF)) {
        return NULL;
    }

    /* In XML mode, identifiers and other tokens in element body are text content */
    if (check(p, TOK_IDENT) || check(p, TOK_NUMBER) || check(p, TOK_STRING)) {
        advance(p);
        return ast_text(p->arena, p->previous.start, p->previous.length,
                       p->previous.line, p->previous.column);
    }

    /* Try to parse as text */
    return parse_text(p);
}

/* ============ Public API ============ */

void parser_init(Parser *p, const char *source, size_t source_len, Arena *arena, MotErrorList *errors) {
    lexer_init(&p->lexer, source, source_len);
    p->arena = arena;
    p->had_error = false;
    p->panic_mode = false;
    p->errors = errors;

    /* Prime the parser */
    advance(p);
}

AstNode *parser_parse(Parser *p) {
    AstNode *children = NULL;
    AstNode **tail = &children;

    while (!check(p, TOK_EOF)) {
        AstNode *node = parse_node(p);
        if (node) {
            *tail = node;
            tail = &node->next;
        }

        if (p->panic_mode) {
            synchronize(p);
        }
    }

    return ast_document(p->arena, children);
}

AstNode *parser_parse_element(Parser *p) {
    if (!match(p, TOK_LT)) {
        error_current(p, "Expected '<'");
        return NULL;
    }
    return parse_element(p);
}

AstNode *parser_parse_expression(Parser *p) {
    lexer_set_mode(&p->lexer, LEX_MODE_EXPR);
    AstNode *expr = parse_expression(p);
    lexer_set_mode(&p->lexer, LEX_MODE_XML);
    return expr;
}

bool parser_had_error(Parser *p) {
    return p->had_error;
}
