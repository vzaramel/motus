/*
 * Motus Parser
 *
 * Recursive descent parser for the XML-based Motus language.
 */

#ifndef MOT_PARSER_H
#define MOT_PARSER_H

#include "ast.h"
#include "../lexer/lexer.h"
#include "../util/arena.h"

/* Error types */
#ifndef MOT_ERROR_TYPES_DEFINED
#define MOT_ERROR_TYPES_DEFINED
typedef struct {
    const char *message;
    const char *file;
    int line;
    int column;
} MotError;

typedef struct {
    MotError *errors;
    size_t count;
    size_t capacity;
} MotErrorList;
#endif

typedef struct Parser {
    Lexer lexer;
    Arena *arena;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
    MotErrorList *errors;
    int mutation_depth;
} Parser;

/* Initialize parser */
void parser_init(Parser *p, const char *source, size_t source_len, Arena *arena, MotErrorList *errors);

/* Parse the entire document */
AstNode *parser_parse(Parser *p);

/* Parse a single element (for testing) */
AstNode *parser_parse_element(Parser *p);

/* Parse an expression (for testing) */
AstNode *parser_parse_expression(Parser *p);

/* Check if parsing encountered errors */
bool parser_had_error(Parser *p);

#endif /* MOT_PARSER_H */
