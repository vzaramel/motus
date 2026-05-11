/*
 * AST (Abstract Syntax Tree) definitions for Motus
 */

#ifndef MOT_AST_H
#define MOT_AST_H

#include "../lexer/tokens.h"
#include "../util/arena.h"
#include <stdbool.h>

/* Forward declarations */
typedef struct AstNode AstNode;
struct Type;
struct Scope;
struct DepSet;

/* AST Node types */
typedef enum {
    /* Document structure */
    NODE_DOCUMENT,       /* Root node */
    NODE_ELEMENT,        /* HTML/XML element */
    NODE_TEXT,           /* Text content */
    NODE_COMMENT,        /* <!-- comment --> */
    NODE_DOCTYPE,        /* <!DOCTYPE ...> */

    /* Motus constructs */
    NODE_LET,            /* <let name value> */
    NODE_VAR,            /* <var name value> */
    NODE_SET,            /* <set name value> */
    NODE_OUTPUT,         /* <output expr> */
    NODE_IF,             /* <if test> */
    NODE_ELSIF,          /* <elsif test> */
    NODE_ELSE,           /* <else> */
    NODE_FOR,            /* <for item in collection> */
    NODE_MATCH,          /* <match value> */
    NODE_CASE,           /* <case pattern> */
    NODE_DEFCOMP,        /* <defcomp Name |props| slots ...> */
    NODE_MACRO,          /* <macro name |params|> */
    NODE_CHILDREN,       /* <children> */
    NODE_IMPORT,         /* <import ... from "..."> */
    NODE_EXPORT,         /* <export ...> */
    NODE_INTERFACE,      /* <interface Name |props|> */
    NODE_REQUIRE_AUTH,   /* <require-auth role="admin"> */

    /* Mutation constructs */
    NODE_INSERT,         /* <insert into binding> ... </insert> */
    NODE_UPDATE,         /* <update binding where cond> ... </update> */
    NODE_DELETE,         /* <delete from binding where cond> ... </delete> */
    NODE_BOUND_INPUT,    /* <input obj.field /> inside mutation block */

    /* Component parts */
    NODE_PROP_DEF,       /* Property definition in defcomp */
    NODE_SLOT_DEF,       /* Slot definition in defcomp */
    NODE_SLOT_FILL,      /* <@slot-name> content </@slot-name> */
    NODE_STYLE,          /* <style> CSS </style> */
    NODE_SCRIPT,         /* <script> JS </script> */

    /* Expressions */
    NODE_LITERAL,        /* Numbers, strings, bools, null */
    NODE_IDENT,          /* Identifier */
    NODE_BINARY,         /* a + b, a and b, etc. */
    NODE_UNARY,          /* -a, not a */
    NODE_CALL,           /* fn arg1 arg2 */
    NODE_PIPE,           /* expr | fn | fn */
    NODE_MEMBER,         /* a.b */
    NODE_INDEX,          /* a[i] */
    NODE_TERNARY,        /* if a then b else c */
    NODE_ARRAY,          /* [a, b, c] */
    NODE_OBJECT,         /* { key: value } */
    NODE_RANGE,          /* 1..10 */

    /* SQL */
    NODE_SQL,            /* SQL query */
    NODE_SQL_SELECT,     /* SELECT columns */
    NODE_SQL_FROM,       /* FROM table */
    NODE_SQL_JOIN,       /* JOIN clause */
    NODE_SQL_WHERE,      /* WHERE condition */
    NODE_SQL_ORDER,      /* ORDER BY clause */
    NODE_SQL_LIMIT,      /* LIMIT n */
    NODE_SQL_OFFSET,     /* OFFSET n */
    NODE_SQL_COLUMN,     /* Column in select list */
    NODE_SQL_PARAM,      /* :param */

    /* Attributes */
    NODE_ATTR,           /* name=value */
} NodeType;

/* Operator types for binary/unary expressions */
typedef enum {
    OP_ADD,              /* + */
    OP_SUB,              /* - */
    OP_MUL,              /* * */
    OP_DIV,              /* / */
    OP_MOD,              /* % */
    OP_LT,               /* lt */
    OP_GT,               /* gt */
    OP_LTE,              /* lte */
    OP_GTE,              /* gte */
    OP_EQ,               /* eq */
    OP_NEQ,              /* neq */
    OP_AND,              /* and */
    OP_OR,               /* or */
    OP_NOT,              /* not (unary) */
    OP_NEG,              /* - (unary) */
} OpType;

/* Literal types */
typedef enum {
    LIT_STRING,
    LIT_NUMBER,
    LIT_INT,
    LIT_BOOL,
    LIT_NULL,
} LiteralType;

/* AST Node structure */
struct AstNode {
    NodeType type;

    /* Source location */
    int line;
    int column;
    const char *source_path; /* Optional source file path for debug attribution */

    /* Node-specific data */
    union {
        /* NODE_DOCUMENT */
        struct {
            AstNode *children;   /* First child (linked list) */
        } document;

        /* NODE_ELEMENT */
        struct {
            char *tag;           /* Tag name */
            AstNode *attrs;      /* Attributes (linked list) */
            AstNode *children;   /* Child nodes (linked list) */
            bool self_closing;   /* Is it self-closing? */
        } element;

        /* NODE_TEXT, NODE_COMMENT, NODE_DOCTYPE */
        struct {
            char *content;
            size_t length;
        } text;

        /* NODE_ATTR */
        struct {
            char *name;
            AstNode *value;      /* Expression or NULL for boolean attr */
        } attr;

        /* NODE_LET, NODE_VAR */
        struct {
            char *name;
            AstNode *value;      /* Expression, or NULL if dynamic */
            AstNode *sql;        /* SQL query if present */
            AstNode *body;       /* Children (for let scoping) */
            bool dynamic;
            bool single;         /* For SQL: single row result */
        } binding;

        /* NODE_SET */
        struct {
            AstNode *target;     /* What to set (ident or member) */
            AstNode *value;
        } set;

        /* NODE_OUTPUT */
        struct {
            AstNode *expr;
        } output;

        /* NODE_IF, NODE_ELSIF */
        struct {
            AstNode *condition;
            AstNode *then_body;
            AstNode *else_branch; /* Next elsif or else */
        } if_stmt;

        /* NODE_ELSE */
        struct {
            AstNode *body;
        } else_stmt;

        /* NODE_FOR */
        struct {
            char *item;          /* Loop variable name */
            char *index;         /* Index variable (optional) */
            AstNode *iterable;   /* Collection or range */
            AstNode *body;
        } for_loop;

        /* NODE_MATCH */
        struct {
            AstNode *value;
            AstNode *cases;      /* Linked list of case nodes */
        } match;

        /* NODE_CASE */
        struct {
            AstNode *pattern;    /* Pattern to match */
            AstNode *body;
            bool is_default;
        } case_stmt;

        /* NODE_DEFCOMP */
        struct {
            char *name;
            AstNode *props;      /* Linked list of prop defs */
            AstNode *slots;      /* Linked list of slot defs */
            AstNode *body;       /* Component template */
        } defcomp;

        /* NODE_MACRO */
        struct {
            char *name;
            AstNode *params;     /* Parameter list */
            AstNode *body;
        } macro;

        /* NODE_PROP_DEF */
        struct {
            char *name;
            char *type_name;     /* Type annotation (optional) */
            AstNode *default_val;
            bool required;
            bool infer_type;     /* Using := syntax */
        } prop_def;

        /* NODE_SLOT_DEF */
        struct {
            char *name;
            char *interface;     /* Interface name (optional) */
        } slot_def;

        /* NODE_SLOT_FILL */
        struct {
            char *slot_name;
            AstNode *params;     /* Parameters from slot */
            AstNode *content;
        } slot_fill;

        /* NODE_IMPORT */
        struct {
            AstNode *names;      /* Imported names (linked list of idents) */
            char *from_path;
            bool is_external;
            bool is_dynamic;     /* Dynamic: load at edge, Static: inline in bytecode */
            char *as_name;       /* For "import * as X" */
        } import;

        /* NODE_EXPORT */
        struct {
            AstNode *names;
            bool is_default;
        } export;

        /* NODE_INTERFACE */
        struct {
            char *name;
            AstNode *props;
        } interface;

        /* NODE_REQUIRE_AUTH */
        struct {
            char *role;          /* Required role (e.g., "admin"), or NULL for any auth */
        } require_auth;

        /* NODE_INSERT */
        struct {
            char *target;        /* Binding name (e.g., "contacts") */
            AstNode *body;       /* Children (inputs, buttons) */
            bool optimistic;     /* true (default) or false (pessimistic) */
        } insert;

        /* NODE_UPDATE */
        struct {
            char *target;        /* Binding name */
            AstNode *where;      /* WHERE condition expression */
            AstNode *body;
            bool optimistic;
        } update;

        /* NODE_DELETE */
        struct {
            char *target;        /* Binding name */
            AstNode *where;      /* WHERE condition expression */
            AstNode *body;
            bool optimistic;
        } delete_stmt;

        /* NODE_BOUND_INPUT */
        struct {
            char *object_name;   /* "contact" */
            char *field_name;    /* "name" */
            AstNode *attrs;      /* Additional HTML attributes */
        } bound_input;

        /* NODE_STYLE, NODE_SCRIPT */
        struct {
            char *code;
            size_t code_len;
        } embedded;

        /* NODE_LITERAL */
        struct {
            LiteralType lit_type;
            union {
                double number;
                int64_t integer;
                bool boolean;
                struct {
                    char *value;
                    size_t length;
                } string;
            } v;  /* Named union member for C99 compatibility */
        } literal;

        /* NODE_IDENT */
        struct {
            char *name;
        } ident;

        /* NODE_BINARY */
        struct {
            OpType op;
            AstNode *left;
            AstNode *right;
        } binary;

        /* NODE_UNARY */
        struct {
            OpType op;
            AstNode *operand;
        } unary;

        /* NODE_CALL */
        struct {
            char *name;
            AstNode *args;       /* Linked list */
        } call;

        /* NODE_PIPE */
        struct {
            AstNode *input;      /* Input expression */
            AstNode *stages;     /* Linked list of call nodes */
        } pipe;

        /* NODE_MEMBER */
        struct {
            AstNode *object;
            char *member;
        } member;

        /* NODE_INDEX */
        struct {
            AstNode *object;
            AstNode *index;
        } index;

        /* NODE_TERNARY */
        struct {
            AstNode *condition;
            AstNode *then_expr;
            AstNode *else_expr;
        } ternary;

        /* NODE_ARRAY */
        struct {
            AstNode *elements;   /* Linked list */
        } array;

        /* NODE_OBJECT */
        struct {
            AstNode *pairs;      /* Linked list of key-value pairs */
        } object;

        /* NODE_RANGE */
        struct {
            AstNode *start;
            AstNode *end;
        } range;

        /* NODE_SQL */
        struct {
            AstNode *columns;    /* SELECT columns */
            AstNode *from;       /* FROM table */
            AstNode *joins;      /* JOIN clauses */
            AstNode *where;      /* WHERE condition */
            AstNode *order;      /* ORDER BY */
            AstNode *limit;      /* LIMIT */
            AstNode *offset;     /* OFFSET */
        } sql;

        /* NODE_SQL_COLUMN */
        struct {
            AstNode *expr;       /* Column expression */
            char *alias;         /* AS alias (optional) */
        } sql_column;

        /* NODE_SQL_FROM, NODE_SQL_JOIN */
        struct {
            char *table;
            char *alias;
            AstNode *on_condition; /* For JOIN */
            int join_type;       /* LEFT, RIGHT, INNER, etc. */
        } sql_table;

        /* NODE_SQL_ORDER */
        struct {
            AstNode *expr;
            bool descending;
        } sql_order;

        /* NODE_SQL_PARAM */
        struct {
            char *name;          /* Parameter name (after :) */
        } sql_param;

    } data;

    /* Linked list for siblings */
    AstNode *next;

    /* Semantic analysis annotations (filled in later) */
    struct Type *resolved_type;
    struct Scope *scope;
    struct DepSet *deps;
};

/* AST construction functions */
AstNode *ast_node_new(Arena *arena, NodeType type, int line, int col);

/* Document */
AstNode *ast_document(Arena *arena, AstNode *children);

/* Elements */
AstNode *ast_element(Arena *arena, const char *tag, AstNode *attrs, AstNode *children, bool self_closing, int line, int col);
AstNode *ast_text(Arena *arena, const char *content, size_t length, int line, int col);
AstNode *ast_comment(Arena *arena, const char *content, size_t length, int line, int col);
AstNode *ast_attr(Arena *arena, const char *name, AstNode *value, int line, int col);

/* Bindings */
AstNode *ast_let(Arena *arena, const char *name, AstNode *value, AstNode *sql, AstNode *body, bool dynamic, bool single, int line, int col);
AstNode *ast_var(Arena *arena, const char *name, AstNode *value, AstNode *sql, bool dynamic, bool single, int line, int col);
AstNode *ast_set(Arena *arena, AstNode *target, AstNode *value, int line, int col);

/* Control flow */
AstNode *ast_output(Arena *arena, AstNode *expr, int line, int col);
AstNode *ast_if(Arena *arena, AstNode *condition, AstNode *then_body, AstNode *else_branch, int line, int col);
AstNode *ast_elsif(Arena *arena, AstNode *condition, AstNode *then_body, AstNode *else_branch, int line, int col);
AstNode *ast_else(Arena *arena, AstNode *body, int line, int col);
AstNode *ast_for(Arena *arena, const char *item, const char *index, AstNode *iterable, AstNode *body, int line, int col);
AstNode *ast_match(Arena *arena, AstNode *value, AstNode *cases, int line, int col);
AstNode *ast_case(Arena *arena, AstNode *pattern, AstNode *body, bool is_default, int line, int col);

/* Components */
AstNode *ast_defcomp(Arena *arena, const char *name, AstNode *props, AstNode *slots, AstNode *body, int line, int col);
AstNode *ast_macro(Arena *arena, const char *name, AstNode *params, AstNode *body, int line, int col);
AstNode *ast_prop_def(Arena *arena, const char *name, const char *type_name, AstNode *default_val, bool required, bool infer_type, int line, int col);
AstNode *ast_slot_def(Arena *arena, const char *name, const char *interface, int line, int col);
AstNode *ast_slot_fill(Arena *arena, const char *slot_name, AstNode *params, AstNode *content, int line, int col);
AstNode *ast_children(Arena *arena, int line, int col);

/* Imports/exports */
AstNode *ast_import(Arena *arena, AstNode *names, const char *from_path, bool is_external, bool is_dynamic, const char *as_name, int line, int col);
AstNode *ast_export(Arena *arena, AstNode *names, bool is_default, int line, int col);

/* Auth */
AstNode *ast_require_auth(Arena *arena, const char *role, int line, int col);

/* Mutations */
AstNode *ast_insert(Arena *arena, const char *target, AstNode *body, bool optimistic, int line, int col);
AstNode *ast_update(Arena *arena, const char *target, AstNode *where, AstNode *body, bool optimistic, int line, int col);
AstNode *ast_delete_stmt(Arena *arena, const char *target, AstNode *where, AstNode *body, bool optimistic, int line, int col);
AstNode *ast_bound_input(Arena *arena, const char *object_name, const char *field_name, AstNode *attrs, int line, int col);

/* Embedded code */
AstNode *ast_style(Arena *arena, const char *code, size_t length, int line, int col);
AstNode *ast_script(Arena *arena, const char *code, size_t length, int line, int col);

/* Expressions */
AstNode *ast_literal_number(Arena *arena, double value, int line, int col);
AstNode *ast_literal_int(Arena *arena, int64_t value, int line, int col);
AstNode *ast_literal_string(Arena *arena, const char *value, size_t length, int line, int col);
AstNode *ast_literal_bool(Arena *arena, bool value, int line, int col);
AstNode *ast_literal_null(Arena *arena, int line, int col);
AstNode *ast_ident(Arena *arena, const char *name, int line, int col);
AstNode *ast_binary(Arena *arena, OpType op, AstNode *left, AstNode *right, int line, int col);
AstNode *ast_unary(Arena *arena, OpType op, AstNode *operand, int line, int col);
AstNode *ast_call(Arena *arena, const char *name, AstNode *args, int line, int col);
AstNode *ast_pipe(Arena *arena, AstNode *input, AstNode *stages, int line, int col);
AstNode *ast_member(Arena *arena, AstNode *object, const char *member, int line, int col);
AstNode *ast_index(Arena *arena, AstNode *object, AstNode *index_expr, int line, int col);
AstNode *ast_ternary(Arena *arena, AstNode *condition, AstNode *then_expr, AstNode *else_expr, int line, int col);
AstNode *ast_array(Arena *arena, AstNode *elements, int line, int col);
AstNode *ast_object(Arena *arena, AstNode *pairs, int line, int col);
AstNode *ast_range(Arena *arena, AstNode *start, AstNode *end, int line, int col);

/* SQL */
AstNode *ast_sql(Arena *arena, AstNode *columns, AstNode *from, AstNode *joins, AstNode *where, AstNode *order, AstNode *limit, AstNode *offset, int line, int col);
AstNode *ast_sql_param(Arena *arena, const char *name, int line, int col);

/* Utility */
void ast_append_child(AstNode *parent, AstNode *child);
void ast_set_source_path_recursive(AstNode *node, const char *source_path);
void ast_print(AstNode *node, int indent);
const char *node_type_name(NodeType type);

#endif /* MOT_AST_H */
