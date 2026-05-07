/*
 * Source map helpers (VLQ mappings + JSON serialization)
 */

#include "sourcemap.h"

#include <stdlib.h>
#include <string.h>

static const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool ok;
} StrBuf;

static bool builder_reserve(SourceMapBuilder *b, size_t extra) {
    size_t needed;
    size_t next_cap;
    char *next_data;

    if (!b || !b->ok) return false;
    needed = b->len + extra + 1;
    if (needed <= b->cap) return true;

    next_cap = b->cap == 0 ? 64 : b->cap;
    while (next_cap < needed) {
        if (next_cap > ((size_t)-1) / 2) {
            b->ok = false;
            return false;
        }
        next_cap *= 2;
    }

    next_data = (char *)realloc(b->data, next_cap);
    if (!next_data) {
        b->ok = false;
        return false;
    }
    b->data = next_data;
    b->cap = next_cap;
    return true;
}

static bool builder_append_n(SourceMapBuilder *b, const char *s, size_t n) {
    if (!builder_reserve(b, n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool builder_append_char(SourceMapBuilder *b, char c) {
    if (!builder_reserve(b, 1)) return false;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return true;
}

static bool sb_reserve(StrBuf *sb, size_t extra) {
    size_t needed = sb->len + extra + 1;
    size_t next_cap = sb->cap;
    char *next_data;

    if (!sb || !sb->ok) return false;
    if (needed <= sb->cap) return true;

    if (next_cap == 0) next_cap = 64;
    while (next_cap < needed) {
        if (next_cap > ((size_t)-1) / 2) {
            sb->ok = false;
            return false;
        }
        next_cap *= 2;
    }

    next_data = (char *)realloc(sb->data, next_cap);
    if (!next_data) {
        sb->ok = false;
        return false;
    }
    sb->data = next_data;
    sb->cap = next_cap;
    return true;
}

static bool sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (!sb_reserve(sb, n)) return false;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append_char(StrBuf *sb, char c) {
    if (!sb_reserve(sb, 1)) return false;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return true;
}

static bool sb_append_cstr(StrBuf *sb, const char *s) {
    if (!s) s = "";
    return sb_append_n(sb, s, strlen(s));
}

static char hex_digit(unsigned v) {
    return (char)(v < 10 ? ('0' + v) : ('A' + (v - 10)));
}

static bool sb_append_json_escaped(StrBuf *sb, const char *s) {
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    if (!sb_append_char(sb, '"')) return false;

    while (*p) {
        unsigned char c = *p++;
        switch (c) {
            case '"':
                if (!sb_append_n(sb, "\\\"", 2)) return false;
                break;
            case '\\':
                if (!sb_append_n(sb, "\\\\", 2)) return false;
                break;
            case '\n':
                if (!sb_append_n(sb, "\\n", 2)) return false;
                break;
            case '\r':
                if (!sb_append_n(sb, "\\r", 2)) return false;
                break;
            case '\t':
                if (!sb_append_n(sb, "\\t", 2)) return false;
                break;
            default:
                if (c < 0x20) {
                    char esc[6];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = '0';
                    esc[3] = '0';
                    esc[4] = hex_digit((unsigned)((c >> 4) & 0xF));
                    esc[5] = hex_digit((unsigned)(c & 0xF));
                    if (!sb_append_n(sb, esc, sizeof(esc))) return false;
                } else {
                    if (!sb_append_char(sb, (char)c)) return false;
                }
                break;
        }
    }

    return sb_append_char(sb, '"');
}

static uint64_t to_vlq(int32_t value) {
    uint64_t magnitude;
    if (value < 0) {
        magnitude = (uint64_t)(-(int64_t)value);
        return (magnitude << 1) | 1u;
    }
    magnitude = (uint64_t)value;
    return magnitude << 1;
}

bool sourcemap_encode_vlq_to_buffer(int32_t value, char *out, size_t out_cap, size_t *out_len) {
    uint64_t vlq = to_vlq(value);
    size_t n = 0;

    if (!out || out_cap == 0) return false;

    do {
        uint8_t digit = (uint8_t)(vlq & 31u);
        vlq >>= 5;
        if (vlq > 0) digit |= 32u;
        if (n >= out_cap) return false;
        out[n++] = BASE64_CHARS[digit];
    } while (vlq > 0);

    if (out_len) *out_len = n;
    return true;
}

void sourcemap_builder_init(SourceMapBuilder *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->ok = true;
    b->current_generated_line = 0;
    b->last_generated_col = 0;
    b->last_source = 0;
    b->last_source_line = 0;
    b->last_source_col = 0;
    b->last_name = 0;
    b->has_segment_on_line = false;
}

void sourcemap_builder_free(SourceMapBuilder *b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->ok = false;
}

static bool builder_append_vlq(SourceMapBuilder *b, int32_t value) {
    char enc[16];
    size_t n = 0;
    if (!sourcemap_encode_vlq_to_buffer(value, enc, sizeof(enc), &n)) {
        b->ok = false;
        return false;
    }
    return builder_append_n(b, enc, n);
}

bool sourcemap_builder_add_mapping(SourceMapBuilder *b,
                                   int32_t generated_line,
                                   int32_t generated_col,
                                   int32_t source_index,
                                   int32_t source_line,
                                   int32_t source_col,
                                   int32_t name_index) {
    int32_t d_generated_col;
    int32_t d_source;
    int32_t d_source_line;
    int32_t d_source_col;
    int32_t d_name;

    if (!b || !b->ok) return false;
    if (generated_line < 0 || generated_col < 0 || source_index < 0 || source_line < 0 || source_col < 0) {
        b->ok = false;
        return false;
    }
    if (name_index < -1) {
        b->ok = false;
        return false;
    }
    if (generated_line < b->current_generated_line) {
        b->ok = false;
        return false;
    }

    while (b->current_generated_line < generated_line) {
        if (!builder_append_char(b, ';')) {
            b->ok = false;
            return false;
        }
        b->current_generated_line++;
        b->last_generated_col = 0;
        b->has_segment_on_line = false;
    }

    if (generated_col < b->last_generated_col) {
        b->ok = false;
        return false;
    }

    if (b->has_segment_on_line) {
        if (!builder_append_char(b, ',')) {
            b->ok = false;
            return false;
        }
    }

    d_generated_col = generated_col - b->last_generated_col;
    d_source = source_index - b->last_source;
    d_source_line = source_line - b->last_source_line;
    d_source_col = source_col - b->last_source_col;

    if (!builder_append_vlq(b, d_generated_col) ||
        !builder_append_vlq(b, d_source) ||
        !builder_append_vlq(b, d_source_line) ||
        !builder_append_vlq(b, d_source_col)) {
        b->ok = false;
        return false;
    }

    if (name_index >= 0) {
        d_name = name_index - b->last_name;
        if (!builder_append_vlq(b, d_name)) {
            b->ok = false;
            return false;
        }
        b->last_name = name_index;
    }

    b->last_generated_col = generated_col;
    b->last_source = source_index;
    b->last_source_line = source_line;
    b->last_source_col = source_col;
    b->has_segment_on_line = true;
    return true;
}

const char *sourcemap_builder_mappings(const SourceMapBuilder *b) {
    if (!b || !b->ok) return NULL;
    return b->data ? b->data : "";
}

char *sourcemap_render_json(const char *file,
                            const char *const *sources,
                            size_t source_count,
                            const char *const *names,
                            size_t name_count,
                            const char *mappings) {
    StrBuf sb;
    size_t i;

    sb.data = NULL;
    sb.len = 0;
    sb.cap = 0;
    sb.ok = true;

    if (!sb_append_char(&sb, '{') ||
        !sb_append_cstr(&sb, "\"version\":3,") ||
        !sb_append_cstr(&sb, "\"file\":") ||
        !sb_append_json_escaped(&sb, file ? file : "") ||
        !sb_append_cstr(&sb, ",\"sources\":[")) {
        free(sb.data);
        return NULL;
    }

    for (i = 0; i < source_count; i++) {
        if (i > 0 && !sb_append_char(&sb, ',')) {
            free(sb.data);
            return NULL;
        }
        if (!sb_append_json_escaped(&sb, sources ? sources[i] : "")) {
            free(sb.data);
            return NULL;
        }
    }

    if (!sb_append_cstr(&sb, "],\"names\":[")) {
        free(sb.data);
        return NULL;
    }

    for (i = 0; i < name_count; i++) {
        if (i > 0 && !sb_append_char(&sb, ',')) {
            free(sb.data);
            return NULL;
        }
        if (!sb_append_json_escaped(&sb, names ? names[i] : "")) {
            free(sb.data);
            return NULL;
        }
    }

    if (!sb_append_cstr(&sb, "],\"mappings\":") ||
        !sb_append_json_escaped(&sb, mappings ? mappings : "") ||
        !sb_append_char(&sb, '}')) {
        free(sb.data);
        return NULL;
    }

    return sb.data;
}
