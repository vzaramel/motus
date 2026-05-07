/*
 * String utilities implementation
 */

#include "str.h"
#include <string.h>
#include <ctype.h>

StrView sv_from_cstr(const char *cstr) {
    StrView sv;
    sv.data = cstr;
    sv.len = cstr ? strlen(cstr) : 0;
    return sv;
}

StrView sv_from_parts(const char *data, size_t len) {
    StrView sv;
    sv.data = data;
    sv.len = len;
    return sv;
}

bool sv_is_empty(StrView sv) {
    return sv.len == 0 || sv.data == NULL;
}

bool sv_eq(StrView a, StrView b) {
    if (a.len != b.len) return false;
    if (a.len == 0) return true;
    return memcmp(a.data, b.data, a.len) == 0;
}

bool sv_eq_cstr(StrView sv, const char *cstr) {
    return sv_eq(sv, sv_from_cstr(cstr));
}

bool sv_eq_nocase(StrView a, StrView b) {
    if (a.len != b.len) return false;
    for (size_t i = 0; i < a.len; i++) {
        if (tolower((unsigned char)a.data[i]) != tolower((unsigned char)b.data[i])) {
            return false;
        }
    }
    return true;
}

bool sv_starts_with(StrView sv, StrView prefix) {
    if (prefix.len > sv.len) return false;
    return memcmp(sv.data, prefix.data, prefix.len) == 0;
}

bool sv_starts_with_cstr(StrView sv, const char *prefix) {
    return sv_starts_with(sv, sv_from_cstr(prefix));
}

bool sv_ends_with(StrView sv, StrView suffix) {
    if (suffix.len > sv.len) return false;
    return memcmp(sv.data + sv.len - suffix.len, suffix.data, suffix.len) == 0;
}

StrView sv_trim(StrView sv) {
    return sv_trim_right(sv_trim_left(sv));
}

StrView sv_trim_left(StrView sv) {
    while (sv.len > 0 && is_whitespace(sv.data[0])) {
        sv.data++;
        sv.len--;
    }
    return sv;
}

StrView sv_trim_right(StrView sv) {
    while (sv.len > 0 && is_whitespace(sv.data[sv.len - 1])) {
        sv.len--;
    }
    return sv;
}

StrView sv_substr(StrView sv, size_t start, size_t len) {
    if (start >= sv.len) {
        return sv_from_parts(sv.data + sv.len, 0);
    }
    if (start + len > sv.len) {
        len = sv.len - start;
    }
    return sv_from_parts(sv.data + start, len);
}

int sv_find_char(StrView sv, char c) {
    for (size_t i = 0; i < sv.len; i++) {
        if (sv.data[i] == c) return (int)i;
    }
    return -1;
}

int sv_find(StrView sv, StrView needle) {
    if (needle.len == 0) return 0;
    if (needle.len > sv.len) return -1;

    for (size_t i = 0; i <= sv.len - needle.len; i++) {
        if (memcmp(sv.data + i, needle.data, needle.len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool sv_split_on(StrView sv, char delim, StrView *before, StrView *after) {
    int pos = sv_find_char(sv, delim);
    if (pos < 0) {
        *before = sv;
        *after = sv_from_parts(sv.data + sv.len, 0);
        return false;
    }
    *before = sv_from_parts(sv.data, (size_t)pos);
    *after = sv_from_parts(sv.data + pos + 1, sv.len - (size_t)pos - 1);
    return true;
}

bool is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

bool is_ident_start(char c) {
    return is_alpha(c) || c == '_';
}

bool is_ident_cont(char c) {
    return is_alnum(c) || c == '_' || c == '-';
}
