/*
 * CSS Extraction and Scoping
 */

#include "codegen.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Check if we're at a CSS selector start */
static bool is_selector_start(const char *css, size_t pos, size_t len) {
    if (pos >= len) return false;
    char c = css[pos];
    /* Selector can start with: letter, #, ., *, [, : */
    return isalpha(c) || c == '#' || c == '.' || c == '*' || c == '[' || c == ':';
}

/* Skip whitespace */
static size_t skip_whitespace(const char *css, size_t pos, size_t len) {
    while (pos < len && isspace(css[pos])) {
        pos++;
    }
    return pos;
}

/* Skip a CSS comment */
static size_t skip_comment(const char *css, size_t pos, size_t len) {
    if (pos + 1 < len && css[pos] == '/' && css[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < len) {
            if (css[pos] == '*' && css[pos + 1] == '/') {
                return pos + 2;
            }
            pos++;
        }
    }
    return pos;
}

/* Find end of selector (before { or ,) */
static size_t find_selector_end(const char *css, size_t pos, size_t len) {
    int paren_depth = 0;
    int bracket_depth = 0;

    while (pos < len) {
        char c = css[pos];

        if (c == '(') paren_depth++;
        else if (c == ')') paren_depth--;
        else if (c == '[') bracket_depth++;
        else if (c == ']') bracket_depth--;
        else if (paren_depth == 0 && bracket_depth == 0) {
            if (c == '{' || c == ',') {
                return pos;
            }
        }
        pos++;
    }
    return pos;
}

/* Find matching closing brace */
static size_t find_block_end(const char *css, size_t pos, size_t len) {
    int brace_depth = 1;
    pos++;  /* Skip opening { */

    while (pos < len && brace_depth > 0) {
        char c = css[pos];
        if (c == '{') brace_depth++;
        else if (c == '}') brace_depth--;
        else if (c == '/' && pos + 1 < len && css[pos + 1] == '*') {
            pos = skip_comment(css, pos, len);
            continue;
        } else if (c == '"' || c == '\'') {
            /* Skip string */
            char quote = c;
            pos++;
            while (pos < len && css[pos] != quote) {
                if (css[pos] == '\\') pos++;
                pos++;
            }
        }
        pos++;
    }
    return pos;
}

/* Scope a single selector with component prefix */
static void scope_selector(Arena *arena, const char *selector, size_t sel_len,
                          const char *component, char **out, size_t *out_len) {
    /* Skip leading whitespace */
    while (sel_len > 0 && isspace(*selector)) {
        selector++;
        sel_len--;
    }
    /* Skip trailing whitespace */
    while (sel_len > 0 && isspace(selector[sel_len - 1])) {
        sel_len--;
    }

    if (sel_len == 0) {
        *out = "";
        *out_len = 0;
        return;
    }

    /* Generate scoped selector: [data-component="Name"] selector */
    /* Format: [data-component="X"] selector
     * Fixed part: [data-component=""] + space = 20 chars
     * Variable: component length + selector length */
    size_t comp_len = strlen(component);
    size_t max_len = 20 + comp_len + sel_len;

    char *result = arena_alloc(arena, max_len + 1);
    int written = snprintf(result, max_len + 1, "[data-component=\"%s\"] %.*s",
             component, (int)sel_len, selector);

    *out = result;
    *out_len = (size_t)written;
}

/* Scope CSS selectors with component prefix */
char *css_scope(Arena *arena, const char *css, size_t length, const char *component) {
    if (!component || !css || length == 0) {
        /* Return copy without scoping */
        char *result = arena_alloc(arena, length + 1);
        memcpy(result, css, length);
        result[length] = '\0';
        return result;
    }

    /* Allocate buffer for result (may be larger due to added prefixes) */
    size_t result_cap = length * 2;  /* Estimate: up to 2x original size */
    char *result = arena_alloc(arena, result_cap);
    size_t result_len = 0;

    size_t pos = 0;

    while (pos < length) {
        /* Skip whitespace and comments */
        size_t old_pos = pos;
        pos = skip_whitespace(css, pos, length);
        pos = skip_comment(css, pos, length);

        /* Copy whitespace/comments to output */
        if (pos > old_pos) {
            size_t copy_len = pos - old_pos;
            memcpy(result + result_len, css + old_pos, copy_len);
            result_len += copy_len;
        }

        if (pos >= length) break;

        /* Check for @ rules (don't scope these directly) */
        if (css[pos] == '@') {
            /* Find the block or semicolon */
            size_t at_start = pos;
            while (pos < length && css[pos] != '{' && css[pos] != ';') {
                pos++;
            }

            if (pos < length && css[pos] == '{') {
                /* @ rule with block - copy as-is for now (could recursively scope) */
                pos = find_block_end(css, pos, length);
            } else if (pos < length) {
                pos++;  /* Skip semicolon */
            }

            size_t copy_len = pos - at_start;
            memcpy(result + result_len, css + at_start, copy_len);
            result_len += copy_len;
            continue;
        }

        /* Parse selector(s) */
        if (is_selector_start(css, pos, length)) {
            bool first_selector = true;

            while (pos < length) {
                size_t sel_start = pos;
                pos = find_selector_end(css, pos, length);

                if (pos > sel_start) {
                    char *scoped_sel;
                    size_t scoped_len;
                    scope_selector(arena, css + sel_start, pos - sel_start,
                                  component, &scoped_sel, &scoped_len);

                    if (!first_selector) {
                        result[result_len++] = ',';
                        result[result_len++] = ' ';
                    }

                    memcpy(result + result_len, scoped_sel, scoped_len);
                    result_len += scoped_len;
                    first_selector = false;
                }

                if (pos < length && css[pos] == ',') {
                    pos++;
                    pos = skip_whitespace(css, pos, length);
                } else {
                    break;
                }
            }

            /* Copy the declaration block */
            if (pos < length && css[pos] == '{') {
                size_t block_start = pos;
                pos = find_block_end(css, pos, length);

                size_t copy_len = pos - block_start;
                memcpy(result + result_len, css + block_start, copy_len);
                result_len += copy_len;
            }
        } else {
            /* Unknown content - copy as-is */
            result[result_len++] = css[pos++];
        }
    }

    result[result_len] = '\0';
    return result;
}
