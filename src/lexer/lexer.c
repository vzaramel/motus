/*
 * Motus Lexer Implementation
 */

#include "lexer.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Keyword table */
typedef struct {
    const char *name;
    TokenType type;
} Keyword;

static const Keyword mot_keywords[] = {
    /* Motus keywords */
    {"let", TOK_LET},
    {"var", TOK_VAR},
    {"set", TOK_SET},
    {"if", TOK_IF},
    {"elsif", TOK_ELSIF},
    {"else", TOK_ELSE},
    {"for", TOK_FOR},
    {"in", TOK_IN},
    {"match", TOK_MATCH},
    {"case", TOK_CASE},
    {"default", TOK_DEFAULT},
    {"defcomp", TOK_DEFCOMP},
    {"macro", TOK_MACRO},
    {"import", TOK_IMPORT},
    {"export", TOK_EXPORT},
    {"output", TOK_OUTPUT},
    {"children", TOK_CHILDREN},
    {"dynamic", TOK_DYNAMIC},
    {"single", TOK_SINGLE},
    {"interface", TOK_INTERFACE},
    {"slots", TOK_SLOTS},
    {"style", TOK_STYLE},
    {"script", TOK_SCRIPT},
    {"fill", TOK_FILL},
    {"external", TOK_EXTERNAL},
    {"from", TOK_FROM},
    {"as", TOK_AS},
    {"then", TOK_THEN},
    {"require-auth", TOK_REQUIRE_AUTH},

    /* Mutation keywords */
    {"insert", TOK_INSERT},
    {"update", TOK_UPDATE},
    {"delete", TOK_DELETE},
    {"into", TOK_INTO},

    /* SQL keywords */
    {"select", TOK_SELECT},
    {"where", TOK_WHERE},
    {"order", TOK_ORDER},
    {"by", TOK_BY},
    {"limit", TOK_LIMIT},
    {"offset", TOK_OFFSET},
    {"join", TOK_JOIN},
    {"left", TOK_LEFT},
    {"right", TOK_RIGHT},
    {"inner", TOK_INNER},
    {"outer", TOK_OUTER},
    {"on", TOK_ON},
    {"asc", TOK_ASC},
    {"desc", TOK_DESC},
    {"count", TOK_COUNT},
    {"sum", TOK_SUM},
    {"avg", TOK_AVG},
    {"min", TOK_MIN},
    {"max", TOK_MAX},
    {"between", TOK_BETWEEN},
    {"like", TOK_LIKE},
    {"is", TOK_IS},

    /* Comparison keywords */
    {"lt", TOK_LT_KW},
    {"gt", TOK_GT_KW},
    {"lte", TOK_LTE},
    {"gte", TOK_GTE},
    {"eq", TOK_EQ_KW},
    {"neq", TOK_NEQ},

    /* Boolean */
    {"and", TOK_AND},
    {"or", TOK_OR},
    {"not", TOK_NOT},
    {"true", TOK_TRUE},
    {"false", TOK_FALSE},
    {"null", TOK_NULL},

    {NULL, TOK_ERROR}
};

/* Token type names for debugging */
const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_LT: return "LT";
        case TOK_GT: return "GT";
        case TOK_LT_SLASH: return "LT_SLASH";
        case TOK_SLASH_GT: return "SLASH_GT";
        case TOK_EQ: return "EQ";
        case TOK_IDENT: return "IDENT";
        case TOK_STRING: return "STRING";
        case TOK_NUMBER: return "NUMBER";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NULL: return "NULL";
        case TOK_LET: return "LET";
        case TOK_VAR: return "VAR";
        case TOK_SET: return "SET";
        case TOK_IF: return "IF";
        case TOK_ELSIF: return "ELSIF";
        case TOK_ELSE: return "ELSE";
        case TOK_FOR: return "FOR";
        case TOK_IN: return "IN";
        case TOK_MATCH: return "MATCH";
        case TOK_CASE: return "CASE";
        case TOK_DEFAULT: return "DEFAULT";
        case TOK_DEFCOMP: return "DEFCOMP";
        case TOK_MACRO: return "MACRO";
        case TOK_IMPORT: return "IMPORT";
        case TOK_EXPORT: return "EXPORT";
        case TOK_OUTPUT: return "OUTPUT";
        case TOK_CHILDREN: return "CHILDREN";
        case TOK_DYNAMIC: return "DYNAMIC";
        case TOK_SINGLE: return "SINGLE";
        case TOK_INTERFACE: return "INTERFACE";
        case TOK_SLOTS: return "SLOTS";
        case TOK_STYLE: return "STYLE";
        case TOK_SCRIPT: return "SCRIPT";
        case TOK_FILL: return "FILL";
        case TOK_EXTERNAL: return "EXTERNAL";
        case TOK_FROM: return "FROM";
        case TOK_AS: return "AS";
        case TOK_THEN: return "THEN";
        case TOK_REQUIRE_AUTH: return "REQUIRE_AUTH";
        case TOK_INSERT: return "INSERT";
        case TOK_UPDATE: return "UPDATE";
        case TOK_DELETE: return "DELETE";
        case TOK_INTO: return "INTO";
        case TOK_SELECT: return "SELECT";
        case TOK_WHERE: return "WHERE";
        case TOK_ORDER: return "ORDER";
        case TOK_BY: return "BY";
        case TOK_LIMIT: return "LIMIT";
        case TOK_OFFSET: return "OFFSET";
        case TOK_JOIN: return "JOIN";
        case TOK_LEFT: return "LEFT";
        case TOK_RIGHT: return "RIGHT";
        case TOK_INNER: return "INNER";
        case TOK_OUTER: return "OUTER";
        case TOK_ON: return "ON";
        case TOK_ASC: return "ASC";
        case TOK_DESC: return "DESC";
        case TOK_COUNT: return "COUNT";
        case TOK_SUM: return "SUM";
        case TOK_AVG: return "AVG";
        case TOK_MIN: return "MIN";
        case TOK_MAX: return "MAX";
        case TOK_BETWEEN: return "BETWEEN";
        case TOK_LIKE: return "LIKE";
        case TOK_IS: return "IS";
        case TOK_LT_KW: return "LT_KW";
        case TOK_GT_KW: return "GT_KW";
        case TOK_LTE: return "LTE";
        case TOK_GTE: return "GTE";
        case TOK_EQ_KW: return "EQ_KW";
        case TOK_NEQ: return "NEQ";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_NOT: return "NOT";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_PIPE: return "PIPE";
        case TOK_DOT: return "DOT";
        case TOK_COMMA: return "COMMA";
        case TOK_COLON: return "COLON";
        case TOK_COLON_EQ: return "COLON_EQ";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_DOTDOT: return "DOTDOT";
        case TOK_AT: return "AT";
        case TOK_HASH: return "HASH";
        case TOK_TEXT: return "TEXT";
        case TOK_COMMENT: return "COMMENT";
        case TOK_DOCTYPE: return "DOCTYPE";
        case TOK_CDATA: return "CDATA";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

int token_is_keyword(TokenType type) {
    /* Check if type is any keyword (Motus, SQL, boolean literals) */
    return (type >= TOK_TRUE && type <= TOK_NULL) ||      /* true, false, null */
           (type >= TOK_LET && type <= TOK_INTO) ||          /* Motus + mutation keywords */
           (type >= TOK_SELECT && type <= TOK_IS) ||       /* SQL keywords */
           (type >= TOK_LT_KW && type <= TOK_NOT);         /* Comparison/boolean keywords */
}

int token_is_operator(TokenType type) {
    return (type >= TOK_LT_KW && type <= TOK_NOT) ||
           (type >= TOK_PLUS && type <= TOK_PERCENT);
}

void lexer_init(Lexer *lex, const char *source, size_t source_len) {
    lex->source = source;
    lex->source_len = source_len;
    lex->pos = 0;
    lex->line = 1;
    lex->column = 1;
    lex->mode = LEX_MODE_XML;
    lex->angle_bracket_depth = 0;
    lex->in_element_content = false;
    lex->had_error = false;
    lex->error_msg[0] = '\0';
}

static char peek_char(Lexer *lex) {
    if (lex->pos >= lex->source_len) return '\0';
    return lex->source[lex->pos];
}

static char peek_char_n(Lexer *lex, size_t n) {
    if (lex->pos + n >= lex->source_len) return '\0';
    return lex->source[lex->pos + n];
}

static char advance(Lexer *lex) {
    if (lex->pos >= lex->source_len) return '\0';
    char c = lex->source[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->column = 1;
    } else {
        lex->column++;
    }
    return c;
}

static void skip_whitespace(Lexer *lex) {
    while (is_whitespace(peek_char(lex))) {
        advance(lex);
    }
}

static Token make_token(Lexer *lex, TokenType type, const char *start, size_t len, int line, int col) {
    (void)lex;  /* Reserved for future use */
    Token tok;
    tok.type = type;
    tok.start = start;
    tok.length = len;
    tok.line = line;
    tok.column = col;
    return tok;
}

static Token error_token(Lexer *lex, const char *message) {
    lex->had_error = true;
    snprintf(lex->error_msg, sizeof(lex->error_msg), "%s", message);

    Token tok;
    tok.type = TOK_ERROR;
    tok.start = message;
    tok.length = strlen(message);
    tok.line = lex->line;
    tok.column = lex->column;
    return tok;
}

static TokenType check_keyword(const char *start, size_t len) {
    for (const Keyword *kw = mot_keywords; kw->name; kw++) {
        if (strlen(kw->name) == len && memcmp(kw->name, start, len) == 0) {
            return kw->type;
        }
    }
    return TOK_IDENT;
}

static Token scan_identifier(Lexer *lex) {
    const char *start = lex->source + lex->pos;
    int line = lex->line;
    int col = lex->column;

    advance(lex); /* consume first char */

    while (is_ident_cont(peek_char(lex))) {
        advance(lex);
    }

    size_t len = (lex->source + lex->pos) - start;
    TokenType type = check_keyword(start, len);

    return make_token(lex, type, start, len, line, col);
}

static Token scan_number(Lexer *lex) {
    const char *start = lex->source + lex->pos;
    int line = lex->line;
    int col = lex->column;

    /* Integer part */
    while (is_digit(peek_char(lex))) {
        advance(lex);
    }

    /* Decimal part */
    if (peek_char(lex) == '.' && is_digit(peek_char_n(lex, 1))) {
        advance(lex); /* consume '.' */
        while (is_digit(peek_char(lex))) {
            advance(lex);
        }
    }

    /* Exponent part */
    char c = peek_char(lex);
    if (c == 'e' || c == 'E') {
        advance(lex);
        c = peek_char(lex);
        if (c == '+' || c == '-') {
            advance(lex);
        }
        if (!is_digit(peek_char(lex))) {
            return error_token(lex, "Invalid number: expected digit after exponent");
        }
        while (is_digit(peek_char(lex))) {
            advance(lex);
        }
    }

    size_t len = (lex->source + lex->pos) - start;
    return make_token(lex, TOK_NUMBER, start, len, line, col);
}

static Token scan_string(Lexer *lex) {
    char quote = peek_char(lex);
    int line = lex->line;
    int col = lex->column;
    const char *start = lex->source + lex->pos;

    advance(lex); /* consume opening quote */

    while (peek_char(lex) != quote && peek_char(lex) != '\0') {
        if (peek_char(lex) == '\\') {
            advance(lex); /* consume backslash */
            if (peek_char(lex) == '\0') {
                return error_token(lex, "Unterminated string");
            }
        }
        advance(lex);
    }

    if (peek_char(lex) == '\0') {
        return error_token(lex, "Unterminated string");
    }

    advance(lex); /* consume closing quote */

    size_t len = (lex->source + lex->pos) - start;
    return make_token(lex, TOK_STRING, start, len, line, col);
}

static Token scan_comment(Lexer *lex) {
    /* Already consumed '<!--' */
    const char *start = lex->source + lex->pos - 4;
    int line = lex->line;
    int col = lex->column - 4;

    while (lex->pos + 2 < lex->source_len) {
        if (peek_char(lex) == '-' &&
            peek_char_n(lex, 1) == '-' &&
            peek_char_n(lex, 2) == '>') {
            advance(lex);
            advance(lex);
            advance(lex);
            size_t len = (lex->source + lex->pos) - start;
            return make_token(lex, TOK_COMMENT, start, len, line, col);
        }
        advance(lex);
    }

    return error_token(lex, "Unterminated comment");
}

static Token scan_doctype(Lexer *lex) {
    /* Already consumed '<!DOCTYPE' or '<!doctype' */
    const char *start = lex->source + lex->pos - 9;
    int line = lex->line;
    int col = lex->column - 9;

    while (peek_char(lex) != '>' && peek_char(lex) != '\0') {
        advance(lex);
    }

    if (peek_char(lex) == '\0') {
        return error_token(lex, "Unterminated DOCTYPE");
    }

    advance(lex); /* consume '>' */

    size_t len = (lex->source + lex->pos) - start;
    return make_token(lex, TOK_DOCTYPE, start, len, line, col);
}

static Token scan_xml_text(Lexer *lex) {
    const char *start = lex->source + lex->pos;
    int line = lex->line;
    int col = lex->column;

    while (peek_char(lex) != '<' && peek_char(lex) != '\0') {
        advance(lex);
    }

    size_t len = (lex->source + lex->pos) - start;
    return make_token(lex, TOK_TEXT, start, len, line, col);
}

/* Scan in XML mode (default) */
static Token scan_xml(Lexer *lex) {
    if (lexer_at_end(lex)) {
        return make_token(lex, TOK_EOF, lex->source + lex->pos, 0, lex->line, lex->column);
    }

    char c = peek_char(lex);
    int line = lex->line;
    int col = lex->column;

    /* If we're in element content (after '>'), scan text until '<'.
     * This preserves whitespace in element text content. */
    if (lex->in_element_content && c != '<') {
        return scan_xml_text(lex);
    }

    /* Skip whitespace when parsing inside tags (between '<' and '>') */
    skip_whitespace(lex);

    if (lexer_at_end(lex)) {
        return make_token(lex, TOK_EOF, lex->source + lex->pos, 0, lex->line, lex->column);
    }

    c = peek_char(lex);
    line = lex->line;
    col = lex->column;

    /* Check for XML constructs */
    if (c == '<') {
        lex->in_element_content = false;  /* Entering a tag */
        const char *start = lex->source + lex->pos;

        /* Check for comment <!-- */
        if (peek_char_n(lex, 1) == '!' &&
            peek_char_n(lex, 2) == '-' &&
            peek_char_n(lex, 3) == '-') {
            advance(lex); advance(lex); advance(lex); advance(lex);
            return scan_comment(lex);
        }

        /* Check for DOCTYPE <!DOCTYPE */
        if (peek_char_n(lex, 1) == '!' &&
            (peek_char_n(lex, 2) == 'D' || peek_char_n(lex, 2) == 'd')) {
            /* Check for DOCTYPE */
            size_t remaining = lex->source_len - lex->pos;
            if (remaining >= 9 &&
                (memcmp(lex->source + lex->pos, "<!DOCTYPE", 9) == 0 ||
                 memcmp(lex->source + lex->pos, "<!doctype", 9) == 0)) {
                for (int i = 0; i < 9; i++) advance(lex);
                return scan_doctype(lex);
            }
        }

        /* Check for closing tag </ */
        if (peek_char_n(lex, 1) == '/') {
            advance(lex); advance(lex);
            return make_token(lex, TOK_LT_SLASH, start, 2, line, col);
        }

        /* Regular opening < */
        advance(lex);
        return make_token(lex, TOK_LT, start, 1, line, col);
    }

    if (c == '>') {
        advance(lex);
        lex->in_element_content = true;  /* After '>', we're in element content */
        return make_token(lex, TOK_GT, lex->source + lex->pos - 1, 1, line, col);
    }

    if (c == '/') {
        if (peek_char_n(lex, 1) == '>') {
            const char *start = lex->source + lex->pos;
            advance(lex); advance(lex);
            return make_token(lex, TOK_SLASH_GT, start, 2, line, col);
        }
        advance(lex);
        return make_token(lex, TOK_SLASH, lex->source + lex->pos - 1, 1, line, col);
    }

    if (c == '=') {
        advance(lex);
        return make_token(lex, TOK_EQ, lex->source + lex->pos - 1, 1, line, col);
    }

    if (c == '"' || c == '\'') {
        return scan_string(lex);
    }

    if (is_ident_start(c)) {
        return scan_identifier(lex);
    }

    if (is_digit(c)) {
        return scan_number(lex);
    }

    /* Punctuation */
    const char *start = lex->source + lex->pos;
    advance(lex);

    switch (c) {
        case '|': return make_token(lex, TOK_PIPE, start, 1, line, col);
        case '.':
            if (peek_char(lex) == '.') {
                advance(lex);
                return make_token(lex, TOK_DOTDOT, start, 2, line, col);
            }
            return make_token(lex, TOK_DOT, start, 1, line, col);
        case ',': return make_token(lex, TOK_COMMA, start, 1, line, col);
        case ':':
            if (peek_char(lex) == '=') {
                advance(lex);
                return make_token(lex, TOK_COLON_EQ, start, 2, line, col);
            }
            return make_token(lex, TOK_COLON, start, 1, line, col);
        case '[': return make_token(lex, TOK_LBRACKET, start, 1, line, col);
        case ']': return make_token(lex, TOK_RBRACKET, start, 1, line, col);
        case '{': return make_token(lex, TOK_LBRACE, start, 1, line, col);
        case '}': return make_token(lex, TOK_RBRACE, start, 1, line, col);
        case '(': return make_token(lex, TOK_LPAREN, start, 1, line, col);
        case ')': return make_token(lex, TOK_RPAREN, start, 1, line, col);
        case '+': return make_token(lex, TOK_PLUS, start, 1, line, col);
        case '-': return make_token(lex, TOK_MINUS, start, 1, line, col);
        case '*': return make_token(lex, TOK_STAR, start, 1, line, col);
        case '%': return make_token(lex, TOK_PERCENT, start, 1, line, col);
        case '@': return make_token(lex, TOK_AT, start, 1, line, col);
        case '#': return make_token(lex, TOK_HASH, start, 1, line, col);
        default:
            return error_token(lex, "Unexpected character");
    }
}

/* Scan in expression mode */
static Token scan_expr(Lexer *lex) {
    skip_whitespace(lex);

    if (lexer_at_end(lex)) {
        return make_token(lex, TOK_EOF, lex->source + lex->pos, 0, lex->line, lex->column);
    }

    char c = peek_char(lex);
    int line = lex->line;
    int col = lex->column;
    const char *start = lex->source + lex->pos;

    /* Handle angle brackets in expression mode for <fn <arg>> syntax */
    if (c == '<') {
        advance(lex);
        lex->angle_bracket_depth++;
        return make_token(lex, TOK_LT, start, 1, line, col);
    }

    if (c == '>') {
        advance(lex);
        if (lex->angle_bracket_depth > 0) {
            lex->angle_bracket_depth--;
        }
        return make_token(lex, TOK_GT, start, 1, line, col);
    }

    if (c == '"' || c == '\'') {
        return scan_string(lex);
    }

    if (is_ident_start(c)) {
        return scan_identifier(lex);
    }

    if (is_digit(c)) {
        return scan_number(lex);
    }

    /* Operators and punctuation */
    advance(lex);

    switch (c) {
        case '|': return make_token(lex, TOK_PIPE, start, 1, line, col);
        case '.':
            if (peek_char(lex) == '.') {
                advance(lex);
                return make_token(lex, TOK_DOTDOT, start, 2, line, col);
            }
            return make_token(lex, TOK_DOT, start, 1, line, col);
        case ',': return make_token(lex, TOK_COMMA, start, 1, line, col);
        case ':':
            if (peek_char(lex) == '=') {
                advance(lex);
                return make_token(lex, TOK_COLON_EQ, start, 2, line, col);
            }
            return make_token(lex, TOK_COLON, start, 1, line, col);
        case '[': return make_token(lex, TOK_LBRACKET, start, 1, line, col);
        case ']': return make_token(lex, TOK_RBRACKET, start, 1, line, col);
        case '{': return make_token(lex, TOK_LBRACE, start, 1, line, col);
        case '}': return make_token(lex, TOK_RBRACE, start, 1, line, col);
        case '(': return make_token(lex, TOK_LPAREN, start, 1, line, col);
        case ')': return make_token(lex, TOK_RPAREN, start, 1, line, col);
        case '+': return make_token(lex, TOK_PLUS, start, 1, line, col);
        case '-': return make_token(lex, TOK_MINUS, start, 1, line, col);
        case '*': return make_token(lex, TOK_STAR, start, 1, line, col);
        case '/':
            if (peek_char(lex) == '>') {
                advance(lex);
                return make_token(lex, TOK_SLASH_GT, start, 2, line, col);
            }
            return make_token(lex, TOK_SLASH, start, 1, line, col);
        case '%': return make_token(lex, TOK_PERCENT, start, 1, line, col);
        case '@': return make_token(lex, TOK_AT, start, 1, line, col);
        case '#': return make_token(lex, TOK_HASH, start, 1, line, col);
        case '=': return make_token(lex, TOK_EQ, start, 1, line, col);
        default:
            return error_token(lex, "Unexpected character in expression");
    }
}

Token lexer_next(Lexer *lex) {
    switch (lex->mode) {
        case LEX_MODE_XML:
            return scan_xml(lex);
        case LEX_MODE_EXPR:
        case LEX_MODE_SQL:
            return scan_expr(lex);
        case LEX_MODE_STYLE:
        case LEX_MODE_SCRIPT:
            /* For now, just scan as text until closing tag */
            return scan_xml_text(lex);
    }
    return error_token(lex, "Invalid lexer mode");
}

Token lexer_peek(Lexer *lex) {
    /* Save state */
    size_t saved_pos = lex->pos;
    int saved_line = lex->line;
    int saved_col = lex->column;
    int saved_depth = lex->angle_bracket_depth;
    bool saved_in_content = lex->in_element_content;

    Token tok = lexer_next(lex);

    /* Restore state */
    lex->pos = saved_pos;
    lex->line = saved_line;
    lex->column = saved_col;
    lex->angle_bracket_depth = saved_depth;
    lex->in_element_content = saved_in_content;

    return tok;
}

void lexer_set_mode(Lexer *lex, LexerMode mode) {
    lex->mode = mode;
    if (mode != LEX_MODE_EXPR) {
        lex->angle_bracket_depth = 0;
    }
    /* Reset element content flag when switching modes */
    if (mode != LEX_MODE_XML) {
        lex->in_element_content = false;
    }
}

LexerMode lexer_get_mode(Lexer *lex) {
    return lex->mode;
}

bool lexer_at_end(Lexer *lex) {
    return lex->pos >= lex->source_len;
}

int lexer_line(Lexer *lex) {
    return lex->line;
}

int lexer_column(Lexer *lex) {
    return lex->column;
}

bool lexer_had_error(Lexer *lex) {
    return lex->had_error;
}

const char *lexer_error_msg(Lexer *lex) {
    return lex->error_msg;
}

StrView token_text(Token tok) {
    return sv_from_parts(tok.start, tok.length);
}

bool token_eq_cstr(Token tok, const char *str) {
    return sv_eq_cstr(token_text(tok), str);
}
