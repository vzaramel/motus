/*
 * Motus Lexer
 *
 * The lexer operates in different modes:
 * - XML mode: Tokenizes XML tags, attributes, text content
 * - Expression mode: Tokenizes expressions (inside <output>, attributes, etc.)
 * - SQL mode: Tokenizes SQL queries (inside <let ... select ...>)
 */

#ifndef MOT_LEXER_H
#define MOT_LEXER_H

#include "tokens.h"
#include "../util/str.h"
#include <stdbool.h>

typedef enum {
    LEX_MODE_XML,        /* Default: parsing XML structure */
    LEX_MODE_EXPR,       /* Inside expressions */
    LEX_MODE_SQL,        /* Inside SQL query */
    LEX_MODE_STYLE,      /* Inside <style> block */
    LEX_MODE_SCRIPT,     /* Inside <script> block */
} LexerMode;

typedef struct {
    const char *source;
    size_t source_len;
    size_t pos;
    int line;
    int column;

    LexerMode mode;
    int angle_bracket_depth;  /* For nested <fn <arg>> syntax */
    bool in_element_content;  /* After '>' and before '<', preserve whitespace */

    /* Error state */
    bool had_error;
    char error_msg[256];
} Lexer;

/* Initialize lexer with source */
void lexer_init(Lexer *lex, const char *source, size_t source_len);

/* Get next token */
Token lexer_next(Lexer *lex);

/* Peek at next token without consuming */
Token lexer_peek(Lexer *lex);

/* Set lexer mode */
void lexer_set_mode(Lexer *lex, LexerMode mode);

/* Get current mode */
LexerMode lexer_get_mode(Lexer *lex);

/* Check if at end of input */
bool lexer_at_end(Lexer *lex);

/* Get current position info */
int lexer_line(Lexer *lex);
int lexer_column(Lexer *lex);

/* Error handling */
bool lexer_had_error(Lexer *lex);
const char *lexer_error_msg(Lexer *lex);

/* Utility: extract token text as string view */
StrView token_text(Token tok);

/* Utility: check if token matches a specific keyword/identifier */
bool token_eq_cstr(Token tok, const char *str);

#endif /* MOT_LEXER_H */
