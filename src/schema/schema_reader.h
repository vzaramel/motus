/*
 * Schema Reader — parses JSON output from capnpc-mot
 *
 * Reads the JSON schema descriptor produced by the capnpc-mot plugin
 * and builds in-memory schema structures for the Motus compiler.
 */

#ifndef MOT_SCHEMA_READER_H
#define MOT_SCHEMA_READER_H

#include "../util/arena.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Annotation on a struct or field */
typedef struct SchemaAnnotation {
    char *name;              /* "label", "computed", "table", etc. */
    char *value;             /* string value, or NULL for bare (true) annotations */
    struct SchemaAnnotation *next;
} SchemaAnnotation;

/* Field in a struct */
typedef struct SchemaField {
    char *name;              /* "email" */
    char *type_name;         /* "Text", "UInt16", "List(Text)", etc. */
    uint16_t ordinal;        /* @N */
    char *default_value;     /* string repr of default, or NULL */
    SchemaAnnotation *annotations;
    struct SchemaField *next;
} SchemaField;

/* Enumerant in an enum */
typedef struct SchemaEnumerant {
    char *name;
    uint16_t ordinal;
    struct SchemaEnumerant *next;
} SchemaEnumerant;

/* Struct definition */
typedef struct SchemaStruct {
    char *name;              /* "Contact" */
    SchemaAnnotation *annotations;  /* struct-level: $table, etc. */
    SchemaField *fields;
    struct SchemaStruct *next;
} SchemaStruct;

/* Enum definition */
typedef struct SchemaEnum {
    char *name;              /* "Role" */
    SchemaEnumerant *enumerants;
    struct SchemaEnum *next;
} SchemaEnum;

/* Parsed schema file */
typedef struct SchemaFile {
    uint64_t file_id;
    SchemaStruct *structs;
    SchemaEnum *enums;
} SchemaFile;

/* Parse JSON output from capnpc-mot into a SchemaFile.
 * All memory is allocated from the arena.
 * Returns NULL on parse error. */
SchemaFile *schema_read_json(const char *json, size_t len, Arena *arena);

/* Look up an annotation value by name. Returns NULL if not found.
 * For bare annotations (Void), returns "" (empty string, non-NULL). */
const char *schema_annotation_get(const SchemaAnnotation *annos, const char *name);

/* Check if an annotation exists */
bool schema_annotation_has(const SchemaAnnotation *annos, const char *name);

/* Find a struct by name in a SchemaFile */
SchemaStruct *schema_find_struct(SchemaFile *file, const char *name);

/* Find an enum by name in a SchemaFile */
SchemaEnum *schema_find_enum(SchemaFile *file, const char *name);

#endif /* MOT_SCHEMA_READER_H */
