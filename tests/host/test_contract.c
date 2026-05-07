#include <stdio.h>
#include <string.h>
#include "../../src/util/arena.h"
#include "../../src/parser/parser.h"
#include "../../src/analyzer/analyzer.h"
#include "../../src/compiler/compiler.h"
#include "../../src/runtime/vm.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int current_test_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("Running " #name "... "); \
    current_test_failed = 0; \
    test_##name(); \
    if (current_test_failed) { \
        tests_failed++; \
        printf("FAILED\n"); \
    } else { \
        tests_passed++; \
        printf("PASSED\n"); \
    } \
} while (0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        current_test_failed = 1; \
        fprintf(stderr, "\nFAILED: %s\n", msg); \
        return; \
    } \
} while (0)

typedef struct {
    VM *vm;
    int called;
    uint32_t expected_query_ref;
    uint32_t expected_signature;
    uint32_t query_ref;
    uint32_t signature;
    uint16_t param_count;
    int saw_category;
    int saw_id;
    int is_single;
    int is_dynamic;
} FetchProbe;

typedef struct {
    VM *vm;
    int called;
    const char *expected_name;
    const char *expected_path;
    int saw_name;
    int saw_path;
    int argc;
    int saw_title;
    int saw_children_null;
} ComponentProbe;

static char output_buf[512];
static size_t output_len = 0;

static void capture_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    if (output_len + len >= sizeof(output_buf)) {
        len = (uint32_t)(sizeof(output_buf) - output_len - 1);
    }
    memcpy(output_buf + output_len, data, len);
    output_len += len;
    output_buf[output_len] = '\0';
}

static int find_param_index(const DataRequirement *req, const char *name) {
    uint16_t i;
    for (i = 0; i < req->param_count; i++) {
        if (strcmp(req->param_names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static Value probe_fetch(const DataRequirement *req, const Value *frame_slots, void *userdata) {
    FetchProbe *probe = (FetchProbe *)userdata;
    probe->called = 1;
    probe->query_ref = req->query_ref;
    probe->signature = req->signature;
    probe->param_count = req->param_count;
    probe->is_single = req->is_single ? 1 : 0;
    probe->is_dynamic = req->is_dynamic ? 1 : 0;

    {
        int idx = find_param_index(req, "category");
        if (idx >= 0) {
            Value v = frame_slots[req->param_slots[idx]];
            if (v.type == VAL_STRING &&
                v.as.string.length == 5 &&
                strncmp(v.as.string.data, "audio", 5) == 0) {
                probe->saw_category = 1;
            }
        }
    }
    {
        int idx = find_param_index(req, "id");
        if (idx >= 0) {
            Value v = frame_slots[req->param_slots[idx]];
            if (v.type == VAL_INT && v.as.integer == 7) {
                probe->saw_id = 1;
            }
        }
    }

    return val_string(probe->vm, "ok", 2);
}

static Value probe_component_load(const ComponentRef *ref, const Value *args,
                                  uint8_t argc, void *userdata) {
    ComponentProbe *probe = (ComponentProbe *)userdata;
    Value title;

    probe->called = 1;
    probe->argc = argc;
    probe->saw_name = (ref->name && strcmp(ref->name, probe->expected_name) == 0) ? 1 : 0;
    probe->saw_path = (ref->path && strcmp(ref->path, probe->expected_path) == 0) ? 1 : 0;

    if (argc >= 1 && args[0].type == VAL_OBJECT) {
        Value *title_ptr = object_get(args[0].as.object, "title");
        if (title_ptr) {
            title = *title_ptr;
            if (title.type == VAL_STRING &&
                title.as.string.length == 4 &&
                strncmp(title.as.string.data, "Home", 4) == 0) {
                probe->saw_title = 1;
            }
        }
    }

    if (argc >= 2 && args[1].type == VAL_NULL) {
        probe->saw_children_null = 1;
    }

    return val_string(probe->vm, "component-ok", 12);
}

static bool resolve_linked_component(const char *path, void *userdata) {
    const char *expected = (const char *)userdata;
    return path && expected && strcmp(path, expected) == 0;
}

TEST(canonical_data_rpc_contract_shape) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<let category \"audio\">"
          "<let id 7>"
            "<let product dynamic single "
              "select * from products where category = :category and id = :id>"
              "<output product>"
            "</let>"
          "</let>"
        "</let>";

    Parser parser;
    AstNode *doc;
    Analyzer *analyzer;
    Compiler *compiler;
    BytecodeModule *module;
    VM *vm;
    VMResult vm_result;
    FetchProbe probe;

    parser_init(&parser, src, strlen(src), arena, NULL);
    doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Parse should succeed");

    analyzer = analyzer_new(arena);
    ASSERT(analyzer_analyze(analyzer, doc), "Analysis should succeed");
    ASSERT(analyzer_ok(analyzer), "Analyzer should report no errors");

    compiler = compiler_new(arena, analyzer);
    module = compiler_compile(compiler, doc);
    ASSERT(module != NULL, "Compile should succeed");
    ASSERT(module->data_req_count == 1, "Expected one data requirement");

    vm = vm_new(arena);
    vm_init(vm, module);

    memset(&probe, 0, sizeof(probe));
    probe.vm = vm;
    probe.expected_query_ref = module->data_reqs[0].query_ref;
    probe.expected_signature = module->data_reqs[0].signature;
    output_buf[0] = '\0';
    output_len = 0;

    vm_set_output(vm, capture_output, NULL);
    vm_set_fetch(vm, probe_fetch, &probe);

    vm_result = vm_run(vm);
    ASSERT(vm_result == VM_OK, "VM execution should succeed");
    ASSERT(probe.called == 1, "Fetch callback should be invoked");
    ASSERT(probe.query_ref == probe.expected_query_ref, "queryRef should match compiled data requirement");
    ASSERT(probe.signature == probe.expected_signature, "signature should match compiled data requirement");
    ASSERT(probe.param_count == 2, "Expected two SQL params");
    ASSERT(probe.saw_category == 1, "Expected category param value from frame slots");
    ASSERT(probe.saw_id == 1, "Expected id param value from frame slots");
    ASSERT(probe.is_single == 1, "Expected single flag in data requirement");
    ASSERT(probe.is_dynamic == 1, "Expected dynamic flag in data requirement");
    ASSERT(strcmp(output_buf, "ok") == 0, "Expected rendered output from fetch result");

    arena_destroy(arena);
}

TEST(component_lookup_path_namespace) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<import Header from \"components/Header\" dynamic>"
        "<Header title=\"Home\" />";

    Parser parser;
    AstNode *doc;
    Analyzer *analyzer;
    Compiler *compiler;
    BytecodeModule *module;
    VM *vm;
    VMResult vm_result;
    ComponentProbe probe;

    parser_init(&parser, src, strlen(src), arena, NULL);
    doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Parse should succeed");

    analyzer = analyzer_new(arena);
    ASSERT(analyzer_analyze(analyzer, doc), "Analysis should succeed");
    ASSERT(analyzer_ok(analyzer), "Analyzer should report no errors");

    compiler = compiler_new(arena, analyzer);
    compiler_set_linked_component_resolver(compiler, resolve_linked_component, (void *)"components/Header");
    module = compiler_compile(compiler, doc);
    ASSERT(module != NULL, "Compile should succeed");
    ASSERT(module->comp_ref_count == 1, "Expected one linked component reference");

    vm = vm_new(arena);
    vm_init(vm, module);

    memset(&probe, 0, sizeof(probe));
    probe.vm = vm;
    probe.expected_name = "Header";
    probe.expected_path = "components/Header";
    output_buf[0] = '\0';
    output_len = 0;

    vm_set_output(vm, capture_output, NULL);
    vm_set_linked_component_loader(vm, probe_component_load, &probe);

    vm_result = vm_run(vm);
    ASSERT(vm_result == VM_OK, "VM execution should succeed");
    ASSERT(probe.called == 1, "Linked component loader should be invoked");
    ASSERT(probe.saw_name == 1, "Linked component name should match import");
    ASSERT(probe.saw_path == 1, "Linked component path should match import namespace");
    ASSERT(probe.argc == 2, "Linked component loader should receive props + children args");
    ASSERT(probe.saw_title == 1, "Linked component props should include title attribute");
    ASSERT(probe.saw_children_null == 1, "Self-closing call should pass null children");
    ASSERT(strcmp(output_buf, "component-ok") == 0, "Rendered linked component output should be emitted");

    arena_destroy(arena);
}

TEST(component_dynamic_load_without_linker) {
    Arena *arena = arena_create(8192);
    const char *src =
        "<import Header from \"components/Header\" dynamic>"
        "<Header title=\"Home\" />";

    Parser parser;
    AstNode *doc;
    Analyzer *analyzer;
    Compiler *compiler;
    BytecodeModule *module;
    VM *vm;
    VMResult vm_result;
    ComponentProbe probe;

    parser_init(&parser, src, strlen(src), arena, NULL);
    doc = parser_parse(&parser);
    ASSERT(doc != NULL, "Parse should succeed");

    analyzer = analyzer_new(arena);
    ASSERT(analyzer_analyze(analyzer, doc), "Analysis should succeed");
    ASSERT(analyzer_ok(analyzer), "Analyzer should report no errors");

    compiler = compiler_new(arena, analyzer);
    module = compiler_compile(compiler, doc);
    ASSERT(module != NULL, "Compile should succeed without linked component resolver");
    ASSERT(module->comp_ref_count == 1, "Expected one component reference");

    vm = vm_new(arena);
    vm_init(vm, module);

    memset(&probe, 0, sizeof(probe));
    probe.vm = vm;
    probe.expected_name = "Header";
    probe.expected_path = "components/Header";
    output_buf[0] = '\0';
    output_len = 0;

    vm_set_output(vm, capture_output, NULL);
    vm_set_component_loader(vm, probe_component_load, &probe);

    vm_result = vm_run(vm);
    ASSERT(vm_result == VM_OK, "VM execution should succeed");
    ASSERT(probe.called == 1, "Dynamic component loader should be invoked");
    ASSERT(probe.saw_name == 1, "Dynamic component name should match import");
    ASSERT(probe.saw_path == 1, "Dynamic component path should match import namespace");
    ASSERT(probe.argc == 2, "Dynamic component loader should receive props + children args");
    ASSERT(probe.saw_title == 1, "Dynamic component props should include title attribute");
    ASSERT(probe.saw_children_null == 1, "Self-closing call should pass null children");
    ASSERT(strcmp(output_buf, "component-ok") == 0, "Rendered dynamic component output should be emitted");

    arena_destroy(arena);
}

int main(void) {
    printf("=== Host Contract Tests ===\n");
    RUN_TEST(canonical_data_rpc_contract_shape);
    RUN_TEST(component_lookup_path_namespace);
    RUN_TEST(component_dynamic_load_without_linker);
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_failed == 0 ? 0 : 1;
}
