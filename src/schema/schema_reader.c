/*
 * Schema Reader — parses JSON output from capnpc-mot
 *
 * Minimal JSON parser for the controlled format produced by capnpc-mot.
 * Not a general-purpose JSON parser — handles only the specific structure
 * we emit: objects, arrays, strings, numbers, booleans, null.
 */

#include "schema_reader.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

/* ================================================================
 * Minimal JSON tokenizer
 * ================================================================ */

typedef enum {
    JTOK_LBRACE, JTOK_RBRACE,
    JTOK_LBRACKET, JTOK_RBRACKET,
    JTOK_COLON, JTOK_COMMA,
    JTOK_STRING, JTOK_NUMBER,
    JTOK_TRUE, JTOK_FALSE, JTOK_NULL,
    JTOK_EOF, JTOK_ERROR
} JTokType;

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    /* Current token */
    JTokType type;
    const char *str_start;
    size_t str_len;
    double num_val;
} JParser;

static void jp_skip_ws(JParser *p) {
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos]))
        p->pos++;
}

static void jp_next(JParser *p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) { p->type = JTOK_EOF; return; }

    char c = p->src[p->pos];
    switch (c) {
        case '{': p->type = JTOK_LBRACE;   p->pos++; return;
        case '}': p->type = JTOK_RBRACE;   p->pos++; return;
        case '[': p->type = JTOK_LBRACKET;  p->pos++; return;
        case ']': p->type = JTOK_RBRACKET;  p->pos++; return;
        case ':': p->type = JTOK_COLON;     p->pos++; return;
        case ',': p->type = JTOK_COMMA;     p->pos++; return;
        case '"': {
            p->pos++; /* skip opening quote */
            p->str_start = p->src + p->pos;
            size_t start = p->pos;
            while (p->pos < p->len && p->src[p->pos] != '"') {
                if (p->src[p->pos] == '\\') p->pos++; /* skip escaped char */
                p->pos++;
            }
            p->str_len = p->pos - start;
            if (p->pos < p->len) p->pos++; /* skip closing quote */
            p->type = JTOK_STRING;
            return;
        }
        default: break;
    }

    /* true/false/null */
    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "true", 4) == 0 &&
        (p->pos + 4 >= p->len || !isalnum((unsigned char)p->src[p->pos + 4]))) {
        p->type = JTOK_TRUE; p->pos += 4; return;
    }
    if (p->pos + 5 <= p->len && memcmp(p->src + p->pos, "false", 5) == 0 &&
        (p->pos + 5 >= p->len || !isalnum((unsigned char)p->src[p->pos + 5]))) {
        p->type = JTOK_FALSE; p->pos += 5; return;
    }
    if (p->pos + 4 <= p->len && memcmp(p->src + p->pos, "null", 4) == 0 &&
        (p->pos + 4 >= p->len || !isalnum((unsigned char)p->src[p->pos + 4]))) {
        p->type = JTOK_NULL; p->pos += 4; return;
    }

    /* Number */
    if (c == '-' || isdigit((unsigned char)c)) {
        const char *start = p->src + p->pos;
        char *end;
        p->num_val = strtod(start, &end);
        p->pos += (size_t)(end - start);
        p->type = JTOK_NUMBER;
        return;
    }

    p->type = JTOK_ERROR;
}

/* Copy a JSON string, handling basic escapes */
static char *jp_copy_str(JParser *p, Arena *arena) {
    char *buf = (char *)arena_alloc(arena, p->str_len + 1);
    if (!buf) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < p->str_len; i++) {
        if (p->str_start[i] == '\\' && i + 1 < p->str_len) {
            i++;
            switch (p->str_start[i]) {
                case '"':  buf[out++] = '"'; break;
                case '\\': buf[out++] = '\\'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 't':  buf[out++] = '\t'; break;
                default:   buf[out++] = p->str_start[i]; break;
            }
        } else {
            buf[out++] = p->str_start[i];
        }
    }
    buf[out] = '\0';
    return buf;
}

/* Skip an arbitrary JSON value (for unknown keys) */
static void jp_skip_value(JParser *p) {
    switch (p->type) {
        case JTOK_LBRACE:
            jp_next(p);
            while (p->type != JTOK_RBRACE && p->type != JTOK_EOF) {
                jp_next(p); /* key */
                if (p->type == JTOK_COLON) jp_next(p);
                jp_skip_value(p);
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACE) jp_next(p);
            break;
        case JTOK_LBRACKET:
            jp_next(p);
            while (p->type != JTOK_RBRACKET && p->type != JTOK_EOF) {
                jp_skip_value(p);
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACKET) jp_next(p);
            break;
        default:
            jp_next(p);
            break;
    }
}

/* ================================================================
 * Schema JSON parsing
 * ================================================================ */

/* Parse annotations object: {"name": "value", "bare": true, ...} */
static SchemaAnnotation *parse_annotations(JParser *p, Arena *arena) {
    if (p->type != JTOK_LBRACE) { jp_skip_value(p); return NULL; }
    jp_next(p); /* skip { */

    SchemaAnnotation *head = NULL;
    SchemaAnnotation **tail = &head;

    while (p->type == JTOK_STRING) {
        SchemaAnnotation *anno = (SchemaAnnotation *)arena_alloc(arena, sizeof(SchemaAnnotation));
        memset(anno, 0, sizeof(*anno));
        anno->name = jp_copy_str(p, arena);
        jp_next(p); /* advance past key */

        if (p->type == JTOK_COLON) jp_next(p);

        if (p->type == JTOK_STRING) {
            anno->value = jp_copy_str(p, arena);
            jp_next(p);
        } else if (p->type == JTOK_TRUE) {
            anno->value = NULL; /* bare annotation */
            jp_next(p);
        } else if (p->type == JTOK_NUMBER) {
            /* Numeric annotation value */
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", p->num_val);
            size_t n = strlen(buf);
            anno->value = (char *)arena_alloc(arena, n + 1);
            memcpy(anno->value, buf, n + 1);
            jp_next(p);
        } else {
            jp_skip_value(p);
        }

        *tail = anno;
        tail = &anno->next;

        if (p->type == JTOK_COMMA) jp_next(p);
    }

    if (p->type == JTOK_RBRACE) jp_next(p);
    return head;
}

/* Parse a single field object */
static SchemaField *parse_field(JParser *p, Arena *arena) {
    if (p->type != JTOK_LBRACE) { jp_skip_value(p); return NULL; }
    jp_next(p);

    SchemaField *field = (SchemaField *)arena_alloc(arena, sizeof(SchemaField));
    memset(field, 0, sizeof(*field));

    while (p->type == JTOK_STRING) {
        char key[64];
        size_t klen = p->str_len < 63 ? p->str_len : 63;
        memcpy(key, p->str_start, klen);
        key[klen] = '\0';
        jp_next(p); /* past key */
        if (p->type == JTOK_COLON) jp_next(p);

        if (strcmp(key, "name") == 0 && p->type == JTOK_STRING) {
            field->name = jp_copy_str(p, arena);
            jp_next(p);
        } else if (strcmp(key, "ordinal") == 0 && p->type == JTOK_NUMBER) {
            field->ordinal = (uint16_t)p->num_val;
            jp_next(p);
        } else if (strcmp(key, "type") == 0 && p->type == JTOK_STRING) {
            field->type_name = jp_copy_str(p, arena);
            jp_next(p);
        } else if (strcmp(key, "default") == 0) {
            if (p->type == JTOK_STRING) {
                field->default_value = jp_copy_str(p, arena);
                jp_next(p);
            } else if (p->type == JTOK_NUMBER) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%g", p->num_val);
                size_t n = strlen(buf);
                field->default_value = (char *)arena_alloc(arena, n + 1);
                memcpy(field->default_value, buf, n + 1);
                jp_next(p);
            } else if (p->type == JTOK_TRUE) {
                field->default_value = (char *)arena_alloc(arena, 5);
                memcpy(field->default_value, "true", 5);
                jp_next(p);
            } else if (p->type == JTOK_FALSE) {
                field->default_value = (char *)arena_alloc(arena, 6);
                memcpy(field->default_value, "false", 6);
                jp_next(p);
            } else {
                jp_skip_value(p);
            }
        } else if (strcmp(key, "annotations") == 0) {
            field->annotations = parse_annotations(p, arena);
        } else {
            jp_skip_value(p);
        }

        if (p->type == JTOK_COMMA) jp_next(p);
    }

    if (p->type == JTOK_RBRACE) jp_next(p);
    return field;
}

/* Parse a struct object */
static SchemaStruct *parse_struct(JParser *p, Arena *arena) {
    if (p->type != JTOK_LBRACE) { jp_skip_value(p); return NULL; }
    jp_next(p);

    SchemaStruct *s = (SchemaStruct *)arena_alloc(arena, sizeof(SchemaStruct));
    memset(s, 0, sizeof(*s));

    while (p->type == JTOK_STRING) {
        char key[64];
        size_t klen = p->str_len < 63 ? p->str_len : 63;
        memcpy(key, p->str_start, klen);
        key[klen] = '\0';
        jp_next(p);
        if (p->type == JTOK_COLON) jp_next(p);

        if (strcmp(key, "name") == 0 && p->type == JTOK_STRING) {
            s->name = jp_copy_str(p, arena);
            jp_next(p);
        } else if (strcmp(key, "annotations") == 0) {
            s->annotations = parse_annotations(p, arena);
        } else if (strcmp(key, "fields") == 0) {
            if (p->type != JTOK_LBRACKET) { jp_skip_value(p); goto next; }
            jp_next(p);
            SchemaField **tail = &s->fields;
            while (p->type != JTOK_RBRACKET && p->type != JTOK_EOF) {
                SchemaField *f = parse_field(p, arena);
                if (f) { *tail = f; tail = &f->next; }
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACKET) jp_next(p);
        } else {
            jp_skip_value(p);
        }

next:
        if (p->type == JTOK_COMMA) jp_next(p);
    }

    if (p->type == JTOK_RBRACE) jp_next(p);
    return s;
}

/* Parse an enum object */
static SchemaEnum *parse_enum(JParser *p, Arena *arena) {
    if (p->type != JTOK_LBRACE) { jp_skip_value(p); return NULL; }
    jp_next(p);

    SchemaEnum *e = (SchemaEnum *)arena_alloc(arena, sizeof(SchemaEnum));
    memset(e, 0, sizeof(*e));

    while (p->type == JTOK_STRING) {
        char key[64];
        size_t klen = p->str_len < 63 ? p->str_len : 63;
        memcpy(key, p->str_start, klen);
        key[klen] = '\0';
        jp_next(p);
        if (p->type == JTOK_COLON) jp_next(p);

        if (strcmp(key, "name") == 0 && p->type == JTOK_STRING) {
            e->name = jp_copy_str(p, arena);
            jp_next(p);
        } else if (strcmp(key, "enumerants") == 0) {
            if (p->type != JTOK_LBRACKET) { jp_skip_value(p); goto enext; }
            jp_next(p);
            SchemaEnumerant **tail = &e->enumerants;
            while (p->type != JTOK_RBRACKET && p->type != JTOK_EOF) {
                if (p->type != JTOK_LBRACE) { jp_skip_value(p); continue; }
                jp_next(p);
                SchemaEnumerant *en = (SchemaEnumerant *)arena_alloc(arena, sizeof(SchemaEnumerant));
                memset(en, 0, sizeof(*en));
                while (p->type == JTOK_STRING) {
                    char ekey[64];
                    size_t eklen = p->str_len < 63 ? p->str_len : 63;
                    memcpy(ekey, p->str_start, eklen);
                    ekey[eklen] = '\0';
                    jp_next(p);
                    if (p->type == JTOK_COLON) jp_next(p);
                    if (strcmp(ekey, "name") == 0 && p->type == JTOK_STRING) {
                        en->name = jp_copy_str(p, arena);
                        jp_next(p);
                    } else if (strcmp(ekey, "ordinal") == 0 && p->type == JTOK_NUMBER) {
                        en->ordinal = (uint16_t)p->num_val;
                        jp_next(p);
                    } else {
                        jp_skip_value(p);
                    }
                    if (p->type == JTOK_COMMA) jp_next(p);
                }
                if (p->type == JTOK_RBRACE) jp_next(p);
                *tail = en;
                tail = &en->next;
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACKET) jp_next(p);
        } else {
            jp_skip_value(p);
        }

enext:
        if (p->type == JTOK_COMMA) jp_next(p);
    }

    if (p->type == JTOK_RBRACE) jp_next(p);
    return e;
}

/* ================================================================
 * Public API
 * ================================================================ */

SchemaFile *schema_read_json(const char *json, size_t len, Arena *arena) {
    if (!json || len == 0 || !arena) return NULL;

    JParser parser;
    memset(&parser, 0, sizeof(parser));
    parser.src = json;
    parser.len = len;
    parser.pos = 0;
    jp_next(&parser);

    JParser *p = &parser;

    if (p->type != JTOK_LBRACE) return NULL;
    jp_next(p);

    SchemaFile *file = (SchemaFile *)arena_alloc(arena, sizeof(SchemaFile));
    memset(file, 0, sizeof(*file));

    while (p->type == JTOK_STRING) {
        char key[64];
        size_t klen = p->str_len < 63 ? p->str_len : 63;
        memcpy(key, p->str_start, klen);
        key[klen] = '\0';
        jp_next(p);
        if (p->type == JTOK_COLON) jp_next(p);

        if (strcmp(key, "fileId") == 0 && p->type == JTOK_STRING) {
            /* Parse hex string "0x..." */
            char *id_str = jp_copy_str(p, arena);
            if (id_str) file->file_id = strtoull(id_str, NULL, 16);
            jp_next(p);
        } else if (strcmp(key, "structs") == 0) {
            if (p->type != JTOK_LBRACKET) { jp_skip_value(p); goto cont; }
            jp_next(p);
            SchemaStruct **tail = &file->structs;
            while (p->type != JTOK_RBRACKET && p->type != JTOK_EOF) {
                SchemaStruct *s = parse_struct(p, arena);
                if (s) { *tail = s; tail = &s->next; }
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACKET) jp_next(p);
        } else if (strcmp(key, "enums") == 0) {
            if (p->type != JTOK_LBRACKET) { jp_skip_value(p); goto cont; }
            jp_next(p);
            SchemaEnum **tail = &file->enums;
            while (p->type != JTOK_RBRACKET && p->type != JTOK_EOF) {
                SchemaEnum *e = parse_enum(p, arena);
                if (e) { *tail = e; tail = &e->next; }
                if (p->type == JTOK_COMMA) jp_next(p);
            }
            if (p->type == JTOK_RBRACKET) jp_next(p);
        } else {
            jp_skip_value(p);
        }

cont:
        if (p->type == JTOK_COMMA) jp_next(p);
    }

    return file;
}

const char *schema_annotation_get(const SchemaAnnotation *annos, const char *name) {
    while (annos) {
        if (strcmp(annos->name, name) == 0) {
            return annos->value ? annos->value : "";
        }
        annos = annos->next;
    }
    return NULL;
}

bool schema_annotation_has(const SchemaAnnotation *annos, const char *name) {
    return schema_annotation_get(annos, name) != NULL;
}

SchemaStruct *schema_find_struct(SchemaFile *file, const char *name) {
    if (!file || !name) return NULL;
    SchemaStruct *s = file->structs;
    while (s) {
        if (s->name && strcmp(s->name, name) == 0) return s;
        s = s->next;
    }
    return NULL;
}

SchemaEnum *schema_find_enum(SchemaFile *file, const char *name) {
    if (!file || !name) return NULL;
    SchemaEnum *e = file->enums;
    while (e) {
        if (e->name && strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    return NULL;
}
