/*
 * String utilities
 */

#ifndef MOT_STR_H
#define MOT_STR_H

#include <stddef.h>
#include <stdbool.h>

/* String view - non-owning reference to a string */
typedef struct {
    const char *data;
    size_t len;
} StrView;

/* Create string view from C string */
StrView sv_from_cstr(const char *cstr);

/* Create string view from pointer and length */
StrView sv_from_parts(const char *data, size_t len);

/* Check if string view is empty */
bool sv_is_empty(StrView sv);

/* Compare two string views */
bool sv_eq(StrView a, StrView b);

/* Compare string view with C string */
bool sv_eq_cstr(StrView sv, const char *cstr);

/* Case-insensitive comparison */
bool sv_eq_nocase(StrView a, StrView b);

/* Check if sv starts with prefix */
bool sv_starts_with(StrView sv, StrView prefix);

/* Check if sv starts with C string prefix */
bool sv_starts_with_cstr(StrView sv, const char *prefix);

/* Check if sv ends with suffix */
bool sv_ends_with(StrView sv, StrView suffix);

/* Trim whitespace from both ends */
StrView sv_trim(StrView sv);

/* Trim whitespace from left */
StrView sv_trim_left(StrView sv);

/* Trim whitespace from right */
StrView sv_trim_right(StrView sv);

/* Get substring */
StrView sv_substr(StrView sv, size_t start, size_t len);

/* Find character in string view, returns index or -1 */
int sv_find_char(StrView sv, char c);

/* Find substring, returns index or -1 */
int sv_find(StrView sv, StrView needle);

/* Split on first occurrence of delimiter */
bool sv_split_on(StrView sv, char delim, StrView *before, StrView *after);

/* Check if character is whitespace */
bool is_whitespace(char c);

/* Check if character is alphabetic */
bool is_alpha(char c);

/* Check if character is digit */
bool is_digit(char c);

/* Check if character is alphanumeric */
bool is_alnum(char c);

/* Check if character is valid identifier start */
bool is_ident_start(char c);

/* Check if character is valid identifier continuation */
bool is_ident_cont(char c);

#endif /* MOT_STR_H */
