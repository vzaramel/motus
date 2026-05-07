/*
 * Lexer tests
 */

#include <stdio.h>
#include <string.h>
#include "lexer/lexer.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s...", #name); \
    tests_run++; \
    test_##name(); \
    tests_passed++; \
    printf(" PASSED\n"); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf(" FAILED: %s\n", msg); \
        return; \
    } \
} while(0)

TEST(simple_html) {
    const char *src = "<div class=\"test\">Hello</div>";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LT, "Expected '<'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_IDENT, "Expected 'div'");
    ASSERT(strncmp(tok.start, "div", 3) == 0, "Expected 'div'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_IDENT, "Expected 'class'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_EQ, "Expected '='");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_STRING, "Expected string");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_GT, "Expected '>'");
}

TEST(mot_keywords) {
    const char *src = "<let name \"value\">";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LT, "Expected '<'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LET, "Expected 'let' keyword");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_IDENT, "Expected identifier");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_STRING, "Expected string");
}

TEST(sql_keywords) {
    const char *src = "select from where order by limit";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_SELECT, "Expected 'select'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_FROM, "Expected 'from'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_WHERE, "Expected 'where'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_ORDER, "Expected 'order'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_BY, "Expected 'by'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LIMIT, "Expected 'limit'");
}

TEST(numbers) {
    const char *src = "42 3.14 1e10";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_NUMBER, "Expected number");
    ASSERT(strncmp(tok.start, "42", 2) == 0, "Expected '42'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_NUMBER, "Expected number");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_NUMBER, "Expected number");
}

TEST(operators) {
    const char *src = "+ - * / % | . ..";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    ASSERT(lexer_next(&lex).type == TOK_PLUS, "Expected '+'");
    ASSERT(lexer_next(&lex).type == TOK_MINUS, "Expected '-'");
    ASSERT(lexer_next(&lex).type == TOK_STAR, "Expected '*'");
    ASSERT(lexer_next(&lex).type == TOK_SLASH, "Expected '/'");
    ASSERT(lexer_next(&lex).type == TOK_PERCENT, "Expected '%'");
    ASSERT(lexer_next(&lex).type == TOK_PIPE, "Expected '|'");
    ASSERT(lexer_next(&lex).type == TOK_DOT, "Expected '.'");
    ASSERT(lexer_next(&lex).type == TOK_DOTDOT, "Expected '..'");
}

TEST(comparison_keywords) {
    const char *src = "lt gt lte gte eq neq and or not";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    ASSERT(lexer_next(&lex).type == TOK_LT_KW, "Expected 'lt'");
    ASSERT(lexer_next(&lex).type == TOK_GT_KW, "Expected 'gt'");
    ASSERT(lexer_next(&lex).type == TOK_LTE, "Expected 'lte'");
    ASSERT(lexer_next(&lex).type == TOK_GTE, "Expected 'gte'");
    ASSERT(lexer_next(&lex).type == TOK_EQ_KW, "Expected 'eq'");
    ASSERT(lexer_next(&lex).type == TOK_NEQ, "Expected 'neq'");
    ASSERT(lexer_next(&lex).type == TOK_AND, "Expected 'and'");
    ASSERT(lexer_next(&lex).type == TOK_OR, "Expected 'or'");
    ASSERT(lexer_next(&lex).type == TOK_NOT, "Expected 'not'");
}

TEST(comment) {
    const char *src = "<!-- this is a comment -->";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_COMMENT, "Expected comment");
}

TEST(closing_tag) {
    const char *src = "</div>";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LT_SLASH, "Expected '</'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_IDENT, "Expected 'div'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_GT, "Expected '>'");
}

TEST(self_closing) {
    const char *src = "<br/>";
    Lexer lex;
    lexer_init(&lex, src, strlen(src));

    Token tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_LT, "Expected '<'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_IDENT, "Expected 'br'");

    tok = lexer_next(&lex);
    ASSERT(tok.type == TOK_SLASH_GT, "Expected '/>'");
}

int main(void) {
    printf("=== Lexer Tests ===\n");

    RUN_TEST(simple_html);
    RUN_TEST(mot_keywords);
    RUN_TEST(sql_keywords);
    RUN_TEST(numbers);
    RUN_TEST(operators);
    RUN_TEST(comparison_keywords);
    RUN_TEST(comment);
    RUN_TEST(closing_tag);
    RUN_TEST(self_closing);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
