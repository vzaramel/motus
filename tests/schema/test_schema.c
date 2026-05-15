/*
 * Schema integration tests
 *
 * Tests the JSON schema reader, type mapping, analyzer integration,
 * and annotation handling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schema/schema_reader.h"
#include "analyzer/analyzer.h"
#include "analyzer/types.h"
#include "parser/parser.h"
#include "mot.h"
#include "util/arena.h"

static int tests_run = 0;
static int tests_passed = 0;
static int test_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s...", #name); \
    fflush(stdout); \
    tests_run++; \
    test_failed = 0; \
    test_##name(); \
    if (!test_failed) { \
        tests_passed++; \
        printf(" PASSED\n"); \
    } \
    fflush(stdout); \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf(" FAILED: %s\n", msg); \
        test_failed = 1; \
        return; \
    } \
} while(0)

/* Sample JSON matching capnpc-mot output */
static const char *CONTACT_JSON =
    "{"
    "\"fileId\":\"0x8b678131d14d12e8\","
    "\"structs\":["
    "  {"
    "    \"name\":\"Contact\","
    "    \"annotations\":{\"table\":\"contacts\"},"
    "    \"fields\":["
    "      {\"name\":\"id\",\"ordinal\":0,\"type\":\"UInt64\",\"annotations\":{\"computed\":true}},"
    "      {\"name\":\"name\",\"ordinal\":1,\"type\":\"Text\",\"annotations\":{\"label\":\"Full Name\"}},"
    "      {\"name\":\"email\",\"ordinal\":2,\"type\":\"Text\",\"default\":null,"
    "       \"annotations\":{\"label\":\"Email\",\"placeholder\":\"user@example.com\"}},"
    "      {\"name\":\"age\",\"ordinal\":3,\"type\":\"UInt16\",\"annotations\":{\"formOptional\":true}},"
    "      {\"name\":\"active\",\"ordinal\":4,\"type\":\"Bool\",\"default\":\"true\"},"
    "      {\"name\":\"bio\",\"ordinal\":5,\"type\":\"Text\","
    "       \"annotations\":{\"multiline\":true,\"label\":\"Biography\",\"formOptional\":true}}"
    "    ]"
    "  },"
    "  {"
    "    \"name\":\"Product\","
    "    \"annotations\":{\"table\":\"products\"},"
    "    \"fields\":["
    "      {\"name\":\"id\",\"ordinal\":0,\"type\":\"UInt64\",\"annotations\":{\"computed\":true}},"
    "      {\"name\":\"productName\",\"ordinal\":1,\"type\":\"Text\",\"annotations\":{\"label\":\"Product Name\"}},"
    "      {\"name\":\"price\",\"ordinal\":2,\"type\":\"Float64\",\"annotations\":{\"label\":\"Price\"}},"
    "      {\"name\":\"tags\",\"ordinal\":3,\"type\":\"List(Text)\"}"
    "    ]"
    "  }"
    "],"
    "\"enums\":["
    "  {\"name\":\"Role\",\"enumerants\":["
    "    {\"name\":\"admin\",\"ordinal\":0},"
    "    {\"name\":\"editor\",\"ordinal\":1},"
    "    {\"name\":\"viewer\",\"ordinal\":2}"
    "  ]}"
    "]"
    "}";

/* ---- JSON parsing tests ---- */

TEST(json_parse_structs) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    ASSERT(sf != NULL, "schema_read_json should succeed");
    ASSERT(sf->structs != NULL, "Should have structs");

    SchemaStruct *contact = schema_find_struct(sf, "Contact");
    ASSERT(contact != NULL, "Should find Contact struct");
    ASSERT(strcmp(contact->name, "Contact") == 0, "Name should be Contact");

    SchemaStruct *product = schema_find_struct(sf, "Product");
    ASSERT(product != NULL, "Should find Product struct");

    arena_destroy(a);
}

TEST(json_parse_fields) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    ASSERT(sf != NULL, "Parse should succeed");

    SchemaStruct *contact = schema_find_struct(sf, "Contact");
    ASSERT(contact != NULL, "Should find Contact");

    /* Count fields */
    int count = 0;
    for (SchemaField *f = contact->fields; f; f = f->next) count++;
    ASSERT(count == 6, "Contact should have 6 fields");

    /* Check first field */
    SchemaField *id = contact->fields;
    ASSERT(id != NULL, "First field should exist");
    ASSERT(strcmp(id->name, "id") == 0, "First field should be id");
    ASSERT(strcmp(id->type_name, "UInt64") == 0, "id type should be UInt64");
    ASSERT(id->ordinal == 0, "id ordinal should be 0");

    arena_destroy(a);
}

TEST(json_parse_annotations) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    SchemaStruct *contact = schema_find_struct(sf, "Contact");
    ASSERT(contact != NULL, "Should find Contact");

    /* Struct-level annotation */
    const char *tbl = schema_annotation_get(contact->annotations, "table");
    ASSERT(tbl != NULL, "Contact should have $table annotation");
    ASSERT(strcmp(tbl, "contacts") == 0, "$table should be 'contacts'");

    /* Field-level annotations */
    SchemaField *id = contact->fields;
    ASSERT(schema_annotation_has(id->annotations, "computed"), "id should have $computed");

    /* Find name field */
    SchemaField *name_field = id->next;
    ASSERT(name_field != NULL && strcmp(name_field->name, "name") == 0, "Second field should be name");
    const char *label = schema_annotation_get(name_field->annotations, "label");
    ASSERT(label != NULL && strcmp(label, "Full Name") == 0, "name should have label 'Full Name'");

    /* Find email field */
    SchemaField *email = name_field->next;
    ASSERT(email != NULL && strcmp(email->name, "email") == 0, "Third field should be email");
    const char *ph = schema_annotation_get(email->annotations, "placeholder");
    ASSERT(ph != NULL && strcmp(ph, "user@example.com") == 0, "email placeholder check");

    /* Find bio field */
    SchemaField *age = email->next;
    SchemaField *active = age->next;
    SchemaField *bio = active->next;
    ASSERT(bio != NULL && strcmp(bio->name, "bio") == 0, "bio field check");
    ASSERT(schema_annotation_has(bio->annotations, "multiline"), "bio should have $multiline");
    ASSERT(schema_annotation_has(bio->annotations, "formOptional"), "bio should have $formOptional");

    arena_destroy(a);
}

TEST(json_parse_enums) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    ASSERT(sf != NULL, "Parse should succeed");

    SchemaEnum *role = schema_find_enum(sf, "Role");
    ASSERT(role != NULL, "Should find Role enum");

    int count = 0;
    for (SchemaEnumerant *e = role->enumerants; e; e = e->next) count++;
    ASSERT(count == 3, "Role should have 3 enumerants");

    ASSERT(role->enumerants != NULL, "First enumerant should exist");
    ASSERT(strcmp(role->enumerants->name, "admin") == 0, "First enumerant should be admin");
    ASSERT(role->enumerants->ordinal == 0, "admin ordinal should be 0");

    arena_destroy(a);
}

TEST(json_parse_defaults) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    SchemaStruct *contact = schema_find_struct(sf, "Contact");
    ASSERT(contact != NULL, "Should find Contact");

    /* active field has default "true" */
    SchemaField *f = contact->fields;
    while (f && strcmp(f->name, "active") != 0) f = f->next;
    ASSERT(f != NULL, "Should find active field");
    ASSERT(f->default_value != NULL, "active should have a default");
    ASSERT(strcmp(f->default_value, "true") == 0, "active default should be 'true'");

    arena_destroy(a);
}

TEST(json_parse_list_type) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    SchemaStruct *product = schema_find_struct(sf, "Product");
    ASSERT(product != NULL, "Should find Product");

    SchemaField *f = product->fields;
    while (f && strcmp(f->name, "tags") != 0) f = f->next;
    ASSERT(f != NULL, "Should find tags field");
    ASSERT(strcmp(f->type_name, "List(Text)") == 0, "tags type should be List(Text)");

    arena_destroy(a);
}

TEST(json_parse_null_returns_null) {
    Arena *a = arena_create(4096);
    SchemaFile *sf = schema_read_json(NULL, 0, a);
    ASSERT(sf == NULL, "NULL input should return NULL");

    sf = schema_read_json("", 0, a);
    ASSERT(sf == NULL, "Empty input should return NULL");

    sf = schema_read_json("not json", 8, a);
    ASSERT(sf == NULL, "Invalid JSON should return NULL");

    arena_destroy(a);
}

/* ---- Type mapping tests ---- */

TEST(schema_to_type_basic) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    ASSERT(sf != NULL, "Parse should succeed");

    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    SchemaStruct *contact = schema_find_struct(sf, "Contact");
    Type *t = analyzer_schema_to_type(an, contact);
    ASSERT(t != NULL, "Type should be created");
    ASSERT(t->kind == TYPE_OBJECT, "Contact should map to TYPE_OBJECT");

    /* Check field types */
    Type *id_type = type_field_type(t, "id");
    ASSERT(id_type != NULL, "id field type should exist");
    ASSERT(id_type->kind == TYPE_INT, "UInt64 should map to TYPE_INT");

    Type *name_type = type_field_type(t, "name");
    ASSERT(name_type != NULL, "name field type should exist");
    ASSERT(name_type->kind == TYPE_STRING, "Text should map to TYPE_STRING");

    Type *active_type = type_field_type(t, "active");
    ASSERT(active_type != NULL, "active field type should exist");
    ASSERT(active_type->kind == TYPE_BOOL, "Bool should map to TYPE_BOOL");

    arena_destroy(a);
}

TEST(schema_to_type_float) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    SchemaStruct *product = schema_find_struct(sf, "Product");
    Type *t = analyzer_schema_to_type(an, product);
    ASSERT(t != NULL, "Product type should be created");

    Type *price_type = type_field_type(t, "price");
    ASSERT(price_type != NULL, "price field should exist");
    ASSERT(price_type->kind == TYPE_NUMBER, "Float64 should map to TYPE_NUMBER");

    arena_destroy(a);
}

TEST(schema_to_type_list) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    SchemaStruct *product = schema_find_struct(sf, "Product");
    ASSERT(product != NULL, "Should find Product struct");
    Type *t = analyzer_schema_to_type(an, product);
    ASSERT(t != NULL, "Product type should exist");
    ASSERT(t->kind == TYPE_OBJECT, "Product should be TYPE_OBJECT");

    /* Debug: walk fields */
    for (TypeField *f = t->data.object.fields; f; f = f->next) {
        printf(" [%s:kind=%d]", f->name, f->type->kind);
    }

    Type *tags_type = type_field_type(t, "tags");
    ASSERT(tags_type != NULL, "tags field should exist");
    printf(" tags_kind=%d", tags_type->kind);
    ASSERT(tags_type->kind == TYPE_ARRAY, "List(Text) should map to TYPE_ARRAY");
    ASSERT(tags_type->data.array.element->kind == TYPE_STRING,
           "List(Text) element should be TYPE_STRING");

    arena_destroy(a);
}

/* ---- Analyzer integration tests ---- */

TEST(resolve_type_name_from_schema) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    Type *t = analyzer_resolve_type_name(an, "Contact");
    ASSERT(t != NULL, "Contact should resolve");
    ASSERT(t->kind == TYPE_OBJECT, "Contact should be TYPE_OBJECT");

    Type *unknown = analyzer_resolve_type_name(an, "Nonexistent");
    ASSERT(unknown->kind == TYPE_UNKNOWN, "Nonexistent should be TYPE_UNKNOWN");

    arena_destroy(a);
}

TEST(find_schema_for_table) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    SchemaStruct *s = analyzer_find_schema_for_table(an, "contacts");
    ASSERT(s != NULL, "Should find schema for table 'contacts'");
    ASSERT(strcmp(s->name, "Contact") == 0, "Should be Contact struct");

    SchemaStruct *p = analyzer_find_schema_for_table(an, "products");
    ASSERT(p != NULL, "Should find schema for table 'products'");
    ASSERT(strcmp(p->name, "Product") == 0, "Should be Product struct");

    SchemaStruct *none = analyzer_find_schema_for_table(an, "nonexistent");
    ASSERT(none == NULL, "Should not find nonexistent table");

    arena_destroy(a);
}

TEST(schema_import_registers_types) {
    Arena *a = arena_create(16384);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    ASSERT(sf != NULL, "Parse should succeed");

    /* Compile with schema */
    const char *src = "<import schema \"test.capnp\" />";

    MotCompileOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.target = MOT_TARGET_BYTECODE;
    opts.partial_eval = true;
    opts.include_debug = false;
    opts.schemas = &sf;
    opts.schema_count = 1;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &opts);
    /* Should compile without errors - schema import is valid syntax */
    ASSERT(result.errors.count == 0, "Schema import should compile cleanly");

    mot_result_free(&result);
    arena_destroy(a);
}

TEST(schema_field_access_valid) {
    Arena *a = arena_create(16384);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);

    /* Use schema type as component prop type */
    const char *src =
        "<import schema \"test.capnp\" />"
        "<defcomp Card | contact: Contact |>"
        "  <output contact.name>"
        "  <output contact.email>"
        "</defcomp>";

    MotCompileOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.target = MOT_TARGET_BYTECODE;
    opts.partial_eval = true;
    opts.include_debug = false;
    opts.schemas = &sf;
    opts.schema_count = 1;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &opts);
    ASSERT(result.errors.count == 0, "Valid field access should compile cleanly");

    mot_result_free(&result);
    arena_destroy(a);
}

TEST(schema_field_access_invalid) {
    Arena *a = arena_create(16384);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);

    const char *src =
        "<import schema \"test.capnp\" />"
        "<defcomp Card | contact: Contact |>"
        "  <output contact.phone>"
        "</defcomp>";

    MotCompileOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.target = MOT_TARGET_BYTECODE;
    opts.partial_eval = true;
    opts.include_debug = false;
    opts.schemas = &sf;
    opts.schema_count = 1;

    MotCompileResult result = mot_compile_with_options(src, strlen(src), &opts);
    ASSERT(result.errors.count > 0, "Invalid field 'phone' should produce an error");

    /* Check error message */
    int found = 0;
    for (size_t i = 0; i < result.errors.count; i++) {
        if (strstr(result.errors.errors[i].message, "phone")) {
            found = 1;
            break;
        }
    }
    ASSERT(found, "Error should mention 'phone'");

    mot_result_free(&result);
    arena_destroy(a);
}

TEST(annotation_computed_check) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    SchemaStruct *contact = schema_find_struct(sf, "Contact");

    /* id should be computed */
    SchemaField *id = contact->fields;
    ASSERT(schema_annotation_has(id->annotations, "computed"), "id should be $computed");

    /* name should NOT be computed */
    SchemaField *name_f = id->next;
    ASSERT(!schema_annotation_has(name_f->annotations, "computed"), "name should not be $computed");

    /* Check form-relevant annotations */
    SchemaField *age = name_f->next->next; /* skip email */
    ASSERT(strcmp(age->name, "age") == 0, "Should be age field");
    ASSERT(schema_annotation_has(age->annotations, "formOptional"), "age should be $formOptional");

    arena_destroy(a);
}

TEST(multiple_schemas) {
    Arena *a = arena_create(65536);
    SchemaFile *sf = schema_read_json(CONTACT_JSON, strlen(CONTACT_JSON), a);
    Analyzer *an = analyzer_new(a);
    analyzer_add_schema(an, sf);

    /* Both Contact and Product should resolve */
    Type *contact = analyzer_resolve_type_name(an, "Contact");
    ASSERT(contact->kind == TYPE_OBJECT, "Contact should resolve to TYPE_OBJECT");

    Type *product = analyzer_resolve_type_name(an, "Product");
    ASSERT(product->kind == TYPE_OBJECT, "Product should resolve to TYPE_OBJECT");

    /* They should have different field counts */
    int c_fields = 0, p_fields = 0;
    for (TypeField *f = contact->data.object.fields; f; f = f->next) c_fields++;
    for (TypeField *f = product->data.object.fields; f; f = f->next) p_fields++;
    ASSERT(c_fields != p_fields, "Contact and Product should have different field counts");

    arena_destroy(a);
}

/* ---- Main ---- */

int main(void) {
    /* JSON parsing */
    RUN_TEST(json_parse_structs);
    RUN_TEST(json_parse_fields);
    RUN_TEST(json_parse_annotations);
    RUN_TEST(json_parse_enums);
    RUN_TEST(json_parse_defaults);
    RUN_TEST(json_parse_list_type);
    RUN_TEST(json_parse_null_returns_null);

    /* Type mapping */
    RUN_TEST(schema_to_type_basic);
    RUN_TEST(schema_to_type_float);
    RUN_TEST(schema_to_type_list);

    /* Analyzer integration */
    RUN_TEST(resolve_type_name_from_schema);
    RUN_TEST(find_schema_for_table);
    RUN_TEST(schema_import_registers_types);
    RUN_TEST(schema_field_access_valid);
    RUN_TEST(schema_field_access_invalid);
    RUN_TEST(annotation_computed_check);
    RUN_TEST(multiple_schemas);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
