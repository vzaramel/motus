/*
 * Token types for Motus lexer
 */

#ifndef MOT_TOKENS_H
#define MOT_TOKENS_H

#include <stddef.h>

typedef enum {
    /* XML structure */
    TOK_LT,              /* < */
    TOK_GT,              /* > */
    TOK_LT_SLASH,        /* </ */
    TOK_SLASH_GT,        /* /> */
    TOK_EQ,              /* = */

    /* Literals */
    TOK_IDENT,           /* identifiers */
    TOK_STRING,          /* "string" */
    TOK_NUMBER,          /* 123, 45.67 */
    TOK_TRUE,            /* true */
    TOK_FALSE,           /* false */
    TOK_NULL,            /* null */

    /* Motus keywords */
    TOK_LET,
    TOK_VAR,
    TOK_SET,
    TOK_IF,
    TOK_ELSIF,
    TOK_ELSE,
    TOK_FOR,
    TOK_IN,
    TOK_MATCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_DEFCOMP,
    TOK_MACRO,
    TOK_IMPORT,
    TOK_EXPORT,
    TOK_OUTPUT,
    TOK_CHILDREN,
    TOK_DYNAMIC,
    TOK_SINGLE,
    TOK_INTERFACE,
    TOK_SLOTS,
    TOK_STYLE,
    TOK_SCRIPT,
    TOK_FILL,
    TOK_EXTERNAL,
    TOK_FROM,
    TOK_AS,
    TOK_THEN,            /* for ternary: if x then y else z */
    TOK_REQUIRE_AUTH,    /* require-auth directive */

    /* Mutation keywords */
    TOK_INSERT,          /* insert */
    TOK_UPDATE,          /* update */
    TOK_DELETE,          /* delete */
    TOK_INTO,            /* into */
    TOK_PESSIMISTIC,     /* pessimistic */

    /* SQL keywords */
    TOK_SELECT,
    TOK_WHERE,
    TOK_ORDER,
    TOK_BY,
    TOK_LIMIT,
    TOK_OFFSET,
    TOK_JOIN,
    TOK_LEFT,
    TOK_RIGHT,
    TOK_INNER,
    TOK_OUTER,
    TOK_ON,
    TOK_ASC,
    TOK_DESC,
    TOK_COUNT,
    TOK_SUM,
    TOK_AVG,
    TOK_MIN,
    TOK_MAX,
    TOK_BETWEEN,
    TOK_LIKE,
    TOK_IS,

    /* Comparison (XML-safe keywords) */
    TOK_LT_KW,           /* lt */
    TOK_GT_KW,           /* gt */
    TOK_LTE,             /* lte */
    TOK_GTE,             /* gte */
    TOK_EQ_KW,           /* eq */
    TOK_NEQ,             /* neq */

    /* Boolean operators */
    TOK_AND,
    TOK_OR,
    TOK_NOT,

    /* Arithmetic operators */
    TOK_PLUS,            /* + */
    TOK_MINUS,           /* - */
    TOK_STAR,            /* * */
    TOK_SLASH,           /* / */
    TOK_PERCENT,         /* % */

    /* Other operators and punctuation */
    TOK_PIPE,            /* | */
    TOK_DOT,             /* . */
    TOK_COMMA,           /* , */
    TOK_COLON,           /* : */
    TOK_COLON_EQ,        /* := */
    TOK_LBRACKET,        /* [ */
    TOK_RBRACKET,        /* ] */
    TOK_LBRACE,          /* { */
    TOK_RBRACE,          /* } */
    TOK_LPAREN,          /* ( */
    TOK_RPAREN,          /* ) */
    TOK_DOTDOT,          /* .. */
    TOK_AT,              /* @ */
    TOK_HASH,            /* # */

    /* Special */
    TOK_TEXT,            /* raw text content */
    TOK_COMMENT,         /* <!-- ... --> */
    TOK_DOCTYPE,         /* <!DOCTYPE ...> */
    TOK_CDATA,           /* <![CDATA[...]]> */
    TOK_NEWLINE,         /* significant newline (in some contexts) */
    TOK_EOF,
    TOK_ERROR,
} TokenType;

typedef struct {
    TokenType type;
    const char *start;   /* pointer into source */
    size_t length;
    int line;
    int column;
} Token;

/* Get string name of token type (for debugging) */
const char *token_type_name(TokenType type);

/* Check if token is a keyword */
int token_is_keyword(TokenType type);

/* Check if token is an operator */
int token_is_operator(TokenType type);

#endif /* MOT_TOKENS_H */
