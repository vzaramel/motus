/*
 * Motus Virtual Machine implementation
 */

#include "vm.h"
#include <string.h>
#include <stdio.h>
#ifndef MOT_WASM_FREESTANDING
#include <stdarg.h>
#endif

#define VM_LINKED_NONE 0xFFFFu
#define VM_LINKED_MARKER_PREFIX "@linked_ref:"

/* Hash function for strings */
static uint32_t hash_string(const char *key, uint32_t length) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static bool parse_u32_dec(const char *s, const char **out_end, uint32_t *out) {
    uint64_t v = 0;
    const char *p = s;
    if (!p || *p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return false;
        p++;
    }
    if (out_end) *out_end = p;
    if (out) *out = (uint32_t)v;
    return true;
}

static bool starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s != *prefix) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool parse_linked_marker(const char *s, uint32_t *out_ref, uint32_t *out_func) {
    const char *p = s;
    uint32_t ref_idx = 0;
    uint32_t func_idx = 0;
    if (!p) return false;
    if (!starts_with(p, VM_LINKED_MARKER_PREFIX)) return false;
    p += (sizeof(VM_LINKED_MARKER_PREFIX) - 1u);
    if (!parse_u32_dec(p, &p, &ref_idx)) return false;
    if (*p != ':') return false;
    p++;
    if (!parse_u32_dec(p, &p, &func_idx)) return false;
    if (*p != '\0') return false;
    if (out_ref) *out_ref = ref_idx;
    if (out_func) *out_func = func_idx;
    return true;
}

static void vm_build_linked_component_targets(VM *vm) {
    if (!vm || !vm->module || vm->module->comp_ref_count == 0) {
        vm->linked_component_func_targets = NULL;
        return;
    }
    vm->linked_component_func_targets = arena_alloc(
        vm->arena, sizeof(uint16_t) * vm->module->comp_ref_count
    );
    if (!vm->linked_component_func_targets) return;
    for (uint32_t i = 0; i < vm->module->comp_ref_count; i++) {
        vm->linked_component_func_targets[i] = VM_LINKED_NONE;
    }
    for (uint32_t i = 0; i < vm->module->dep_count; i++) {
        uint32_t ref_idx = 0;
        uint32_t func_idx = 0;
        const char *dep = vm->module->deps[i].path;
        if (!parse_linked_marker(dep, &ref_idx, &func_idx)) continue;
        if (ref_idx >= vm->module->comp_ref_count) continue;
        if (func_idx >= vm->module->func_count) continue;
        vm->linked_component_func_targets[ref_idx] = (uint16_t)func_idx;
    }
}

#ifdef MOT_WASM_FREESTANDING
static int i64_to_buf(int64_t value, char *buf, int cap) {
    char tmp[32];
    int pos = 0;
    int neg = 0;
    uint64_t n;

    if (cap <= 0) return 0;

    if (value < 0) {
        neg = 1;
        n = (uint64_t)(-(value + 1)) + 1u;
    } else {
        n = (uint64_t)value;
    }

    if (n == 0) {
        if (cap > 1) {
            buf[0] = '0';
            buf[1] = '\0';
            return 1;
        }
        buf[0] = '\0';
        return 0;
    }

    while (n > 0 && pos < (int)sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (n % 10u));
        n /= 10u;
    }

    int out = 0;
    if (neg && out < cap - 1) {
        buf[out++] = '-';
    }
    while (pos > 0 && out < cap - 1) {
        buf[out++] = tmp[--pos];
    }
    buf[out] = '\0';
    return out;
}

static int f64_to_buf(double value, char *buf, int cap) {
    /* Freestanding fallback: compact fixed-point up to 6 decimals. */
    if (cap <= 0) return 0;
    if (value != value) {
        if (cap > 4) {
            memcpy(buf, "nan", 4);
            return 3;
        }
        buf[0] = '\0';
        return 0;
    }

    int out = 0;
    double v = value;
    if (v < 0 && out < cap - 1) {
        buf[out++] = '-';
        v = -v;
    }

    int64_t whole = (int64_t)v;
    double frac_d = (v - (double)whole) * 1000000.0 + 0.5;
    if (frac_d >= 1000000.0) {
        whole++;
        frac_d -= 1000000.0;
    }

    out += i64_to_buf(whole, buf + out, cap - out);
    if (out >= cap - 1) return out;

    uint32_t frac = (uint32_t)frac_d;
    if (frac == 0) return out;

    buf[out++] = '.';
    int started = 0;
    uint32_t div = 100000;
    while (div > 0 && out < cap - 1) {
        uint32_t digit = frac / div;
        frac %= div;
        div /= 10;
        if (!started && digit == 0 && div > 0) continue;
        started = 1;
        buf[out++] = (char)('0' + digit);
    }
    buf[out] = '\0';
    return out;
}
#endif

/* VM lifecycle */
VM *vm_new(Arena *arena) {
    VM *vm = arena_alloc(arena, sizeof(VM));
    vm->arena = arena;
    vm->module = NULL;
    vm->stack_top = vm->stack;
    vm->frame_count = 0;
    vm->capture_depth = 0;
    vm->output_fn = NULL;
    vm->output_userdata = NULL;
    vm->fetch_fn = NULL;
    vm->fetch_userdata = NULL;
    vm->component_fn = NULL;
    vm->component_userdata = NULL;
    vm->linked_component_fn = NULL;
    vm->linked_component_userdata = NULL;
    vm->linked_component_func_targets = NULL;
    vm->component_start_fn = NULL;
    vm->component_end_fn = NULL;
    vm->slot_default_start_fn = NULL;
    vm->slot_default_end_fn = NULL;
    vm->component_boundary_userdata = NULL;
    vm->step_hook_fn = NULL;
    vm->step_hook_userdata = NULL;
    vm->awaiting = false;
    vm->globals = NULL;
    vm->had_error = false;
    vm->error_msg[0] = '\0';
    vm->instructions_executed = 0;
    return vm;
}

void vm_init(VM *vm, BytecodeModule *module) {
    vm->module = module;
    vm->stack_top = vm->stack;
    vm->frame_count = 0;
    vm->capture_depth = 0;
    vm->had_error = false;
    vm->error_msg[0] = '\0';
    vm->awaiting = false;
    vm->linked_component_func_targets = NULL;

    /* Set up initial call frame for main chunk */
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = &module->main;
    frame->ip = module->main.code;
    frame->slots = vm->stack;
    frame->kind = VM_FRAME_MAIN;
    frame->func_idx = 0xFFFF;

    /* Create globals object */
    vm->globals = arena_alloc(vm->arena, sizeof(VMObject));
    vm->globals->fields = NULL;
    vm->globals->count = 0;
    vm->globals->capacity = 0;
    vm_build_linked_component_targets(vm);
}

void vm_set_output(VM *vm, VMOutputFn fn, void *userdata) {
    vm->output_fn = fn;
    vm->output_userdata = userdata;
}

void vm_set_fetch(VM *vm, VMFetchDataFn fn, void *userdata) {
    vm->fetch_fn = fn;
    vm->fetch_userdata = userdata;
}

void vm_set_component_loader(VM *vm, VMComponentLoadFn fn, void *userdata) {
    vm->component_fn = fn;
    vm->component_userdata = userdata;
}

void vm_set_linked_component_loader(VM *vm, VMComponentLinkedLoadFn fn, void *userdata) {
    vm->linked_component_fn = fn;
    vm->linked_component_userdata = userdata;
}

void vm_set_component_boundary_hooks(
    VM *vm,
    VMComponentStartFn start_fn,
    VMComponentEndFn end_fn,
    void *userdata
) {
    vm->component_start_fn = start_fn;
    vm->component_end_fn = end_fn;
    vm->component_boundary_userdata = userdata;
}

void vm_set_slot_default_hooks(
    VM *vm,
    VMSlotDefaultStartFn start_fn,
    VMSlotDefaultEndFn end_fn,
    void *userdata
) {
    vm->slot_default_start_fn = start_fn;
    vm->slot_default_end_fn = end_fn;
    vm->component_boundary_userdata = userdata;
}

void vm_set_step_hook(VM *vm, VMStepHookFn fn, void *userdata) {
    vm->step_hook_fn = fn;
    vm->step_hook_userdata = userdata;
}

void vm_request_await(VM *vm) {
    if (vm) vm->awaiting = true;
}

void vm_clear_await(VM *vm) {
    if (vm) vm->awaiting = false;
}

bool vm_is_awaiting(VM *vm) {
    return vm ? vm->awaiting : false;
}

/* Value constructors */
Value val_null(void) {
    Value v;
    v.type = VAL_NULL;
    return v;
}

Value val_bool(bool value) {
    Value v;
    v.type = VAL_BOOL;
    v.as.boolean = value;
    return v;
}

Value val_int(int64_t value) {
    Value v;
    v.type = VAL_INT;
    v.as.integer = value;
    return v;
}

Value val_number(double value) {
    Value v;
    v.type = VAL_NUMBER;
    v.as.number = value;
    return v;
}

Value val_string(VM *vm, const char *data, uint32_t length) {
    Value v;
    v.type = VAL_STRING;
    v.as.string.data = arena_alloc(vm->arena, length + 1);
    memcpy(v.as.string.data, data, length);
    v.as.string.data[length] = '\0';
    v.as.string.length = length;
    v.as.string.hash = hash_string(data, length);
    return v;
}

Value val_array(VM *vm, uint32_t capacity) {
    Value v;
    v.type = VAL_ARRAY;
    v.as.array = arena_alloc(vm->arena, sizeof(VMArray));
    v.as.array->count = 0;
    v.as.array->capacity = capacity;
    v.as.array->elements = capacity > 0 ?
        arena_alloc(vm->arena, sizeof(Value) * capacity) : NULL;
    return v;
}

Value val_object(VM *vm, uint32_t capacity) {
    Value v;
    v.type = VAL_OBJECT;
    v.as.object = arena_alloc(vm->arena, sizeof(VMObject));
    v.as.object->count = 0;
    v.as.object->capacity = capacity;
    v.as.object->fields = capacity > 0 ?
        arena_alloc(vm->arena, sizeof(ObjectField) * capacity) : NULL;
    return v;
}

/* Value operations */
bool val_is_truthy(Value *value) {
    switch (value->type) {
        case VAL_NULL: return false;
        case VAL_BOOL: return value->as.boolean;
        case VAL_INT: return value->as.integer != 0;
        case VAL_NUMBER: return value->as.number != 0.0;
        case VAL_STRING: return value->as.string.length > 0;
        case VAL_ARRAY: return value->as.array->count > 0;
        case VAL_OBJECT: return value->as.object->count > 0;
        case VAL_ITERATOR: return true;
    }
    return false;
}

bool val_equals(Value *a, Value *b) {
    if (a->type != b->type) {
        /* Cross-type numeric comparison */
        double da, db;
        bool a_num = (a->type == VAL_INT || a->type == VAL_NUMBER);
        bool b_num = (b->type == VAL_INT || b->type == VAL_NUMBER);
        if (a_num && b_num) {
            da = (a->type == VAL_INT) ? (double)a->as.integer : a->as.number;
            db = (b->type == VAL_INT) ? (double)b->as.integer : b->as.number;
            return da == db;
        }
        return false;
    }

    switch (a->type) {
        case VAL_NULL: return true;
        case VAL_BOOL: return a->as.boolean == b->as.boolean;
        case VAL_INT: return a->as.integer == b->as.integer;
        case VAL_NUMBER: return a->as.number == b->as.number;
        case VAL_STRING:
            return a->as.string.length == b->as.string.length &&
                   a->as.string.hash == b->as.string.hash &&
                   memcmp(a->as.string.data, b->as.string.data,
                          a->as.string.length) == 0;
        case VAL_ARRAY:
        case VAL_OBJECT:
        case VAL_ITERATOR:
            return false;  /* Reference equality for now */
    }
    return false;
}

int val_compare(Value *a, Value *b) {
    /* Numeric comparison */
    if ((a->type == VAL_INT || a->type == VAL_NUMBER) &&
        (b->type == VAL_INT || b->type == VAL_NUMBER)) {
        double da = (a->type == VAL_INT) ? (double)a->as.integer : a->as.number;
        double db = (b->type == VAL_INT) ? (double)b->as.integer : b->as.number;
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    /* String comparison */
    if (a->type == VAL_STRING && b->type == VAL_STRING) {
        return strcmp(a->as.string.data, b->as.string.data);
    }

    return 0;  /* Incomparable */
}

void val_print(Value *value) {
#ifndef MOT_WASM_FREESTANDING
    switch (value->type) {
        case VAL_NULL: printf("null"); break;
        case VAL_BOOL: printf("%s", value->as.boolean ? "true" : "false"); break;
        case VAL_INT: printf("%lld", (long long)value->as.integer); break;
        case VAL_NUMBER: printf("%g", value->as.number); break;
        case VAL_STRING: printf("\"%s\"", value->as.string.data); break;
        case VAL_ARRAY:
            printf("[array %u]", value->as.array->count);
            break;
        case VAL_OBJECT:
            printf("{object %u}", value->as.object->count);
            break;
        case VAL_ITERATOR:
            printf("<iterator %u/%u>", value->as.iterator->current,
                   value->as.iterator->count);
            break;
    }
#else
    (void)value;
#endif
}

/* Convert value to string for output */
Value val_to_string(VM *vm, Value *value) {
    char buf[64];
    switch (value->type) {
        case VAL_NULL:
            return val_string(vm, "", 0);
        case VAL_BOOL:
            return val_string(vm, value->as.boolean ? "true" : "false",
                              value->as.boolean ? 4 : 5);
        case VAL_INT: {
#ifdef MOT_WASM_FREESTANDING
            int len = i64_to_buf(value->as.integer, buf, (int)sizeof(buf));
#else
            int len = snprintf(buf, sizeof(buf), "%lld",
                               (long long)value->as.integer);
#endif
            return val_string(vm, buf, len);
        }
        case VAL_NUMBER: {
#ifdef MOT_WASM_FREESTANDING
            int len = f64_to_buf(value->as.number, buf, (int)sizeof(buf));
#else
            int len = snprintf(buf, sizeof(buf), "%g", value->as.number);
#endif
            return val_string(vm, buf, len);
        }
        case VAL_STRING:
            return *value;  /* Already a string */
        default:
            return val_string(vm, "", 0);
    }
}

/* Array operations */
void array_push(VM *vm, VMArray *arr, Value value) {
    if (arr->count >= arr->capacity) {
        uint32_t new_cap = arr->capacity < 8 ? 8 : arr->capacity * 2;
        Value *new_elements = arena_alloc(vm->arena, sizeof(Value) * new_cap);
        if (arr->elements) {
            memcpy(new_elements, arr->elements, sizeof(Value) * arr->count);
        }
        arr->elements = new_elements;
        arr->capacity = new_cap;
    }
    arr->elements[arr->count++] = value;
}

Value array_get(VMArray *arr, uint32_t index) {
    if (index >= arr->count) return val_null();
    return arr->elements[index];
}

void array_set(VMArray *arr, uint32_t index, Value value) {
    if (index < arr->count) {
        arr->elements[index] = value;
    }
}

/* Object operations */
void object_set(VM *vm, VMObject *obj, const char *key, Value value) {
    uint32_t key_hash = hash_string(key, (uint32_t)strlen(key));

    /* Check for existing key */
    for (uint32_t i = 0; i < obj->count; i++) {
        if (obj->fields[i].key_hash == key_hash &&
            strcmp(obj->fields[i].key, key) == 0) {
            obj->fields[i].value = value;
            return;
        }
    }

    /* Add new field */
    if (obj->count >= obj->capacity) {
        uint32_t new_cap = obj->capacity < 8 ? 8 : obj->capacity * 2;
        ObjectField *new_fields = arena_alloc(vm->arena,
                                               sizeof(ObjectField) * new_cap);
        if (obj->fields) {
            memcpy(new_fields, obj->fields, sizeof(ObjectField) * obj->count);
        }
        obj->fields = new_fields;
        obj->capacity = new_cap;
    }

    size_t key_len = strlen(key);
    obj->fields[obj->count].key = arena_alloc(vm->arena, key_len + 1);
    memcpy(obj->fields[obj->count].key, key, key_len + 1);
    obj->fields[obj->count].key_hash = key_hash;
    obj->fields[obj->count].value = value;
    obj->count++;
}

Value *object_get(VMObject *obj, const char *key) {
    uint32_t key_hash = hash_string(key, (uint32_t)strlen(key));
    for (uint32_t i = 0; i < obj->count; i++) {
        if (obj->fields[i].key_hash == key_hash &&
            strcmp(obj->fields[i].key, key) == 0) {
            return &obj->fields[i].value;
        }
    }
    return NULL;
}

bool object_has(VMObject *obj, const char *key) {
    return object_get(obj, key) != NULL;
}

/* Global data */
void vm_set_global(VM *vm, const char *name, Value value) {
    if (!vm->globals) {
        vm->globals = arena_alloc(vm->arena, sizeof(VMObject));
        vm->globals->fields = NULL;
        vm->globals->count = 0;
        vm->globals->capacity = 0;
    }
    object_set(vm, vm->globals, name, value);
}

Value *vm_get_global(VM *vm, const char *name) {
    if (!vm->globals) return NULL;
    return object_get(vm->globals, name);
}

/* Error reporting */
void vm_error(VM *vm, const char *fmt, ...) {
#ifdef MOT_WASM_FREESTANDING
    size_t n = strlen(fmt);
    if (n >= sizeof(vm->error_msg)) n = sizeof(vm->error_msg) - 1;
    memcpy(vm->error_msg, fmt, n);
    vm->error_msg[n] = '\0';
#else
    va_list args;
    va_start(args, fmt);
    vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, args);
    va_end(args);
#endif
    vm->had_error = true;
}

const char *vm_error_message(VM *vm) {
    return vm->error_msg;
}

/* Stack operations */
static void push(VM *vm, Value value) {
    if (vm->stack_top - vm->stack >= VM_STACK_MAX) {
        vm_error(vm, "Stack overflow");
        return;
    }
    *vm->stack_top++ = value;
}

static Value pop(VM *vm) {
    if (vm->stack_top == vm->stack) {
        vm_error(vm, "Stack underflow");
        return val_null();
    }
    return *--vm->stack_top;
}

void vm_replace_top(VM *vm, Value value) {
    if (!vm) return;
    if (vm->stack_top == vm->stack) {
        push(vm, value);
        return;
    }
    vm->stack_top[-1] = value;
}

static Value peek(VM *vm, int distance) {
    return vm->stack_top[-1 - distance];
}

/* Read bytes from bytecode */
static uint8_t read_byte(CallFrame *frame) {
    return *frame->ip++;
}

static uint16_t read_u16(CallFrame *frame) {
    frame->ip += 2;
    return (uint16_t)(frame->ip[-2] | (frame->ip[-1] << 8));  /* little-endian */
}

static int16_t read_i16(CallFrame *frame) {
    return (int16_t)read_u16(frame);
}

/* Output helpers */
static void emit_output(VM *vm, const char *data, uint32_t len) {
    if (vm->capture_depth > 0) {
        VMCaptureBuffer *cap = &vm->captures[vm->capture_depth - 1];
        uint32_t need = cap->len + len + 1;
        if (need > cap->cap) {
            uint32_t new_cap = cap->cap ? cap->cap * 2 : 256;
            while (new_cap < need) {
                new_cap *= 2;
            }
            char *new_data = arena_alloc(vm->arena, new_cap);
            if (!new_data) {
                vm_error(vm, "Slot capture allocation failed");
                return;
            }
            if (cap->data && cap->len > 0) {
                memcpy(new_data, cap->data, cap->len);
            }
            cap->data = new_data;
            cap->cap = new_cap;
        }
        memcpy(cap->data + cap->len, data, len);
        cap->len += len;
        cap->data[cap->len] = '\0';
        return;
    }
    if (vm->output_fn) {
        vm->output_fn(data, len, vm->output_userdata);
    }
}

void vm_emit_raw(VM *vm, const char *data, uint32_t len) {
    emit_output(vm, data, len);
}

static void emit_escaped(VM *vm, const char *data, uint32_t len) {
    /* HTML escape: &, <, >, ", ' */
    for (uint32_t i = 0; i < len; i++) {
        char c = data[i];
        switch (c) {
            case '&': emit_output(vm, "&amp;", 5); break;
            case '<': emit_output(vm, "&lt;", 4); break;
            case '>': emit_output(vm, "&gt;", 4); break;
            case '"': emit_output(vm, "&quot;", 6); break;
            case '\'': emit_output(vm, "&#39;", 5); break;
            default: emit_output(vm, &c, 1); break;
        }
    }
}

/* Builtin functions */
static Value builtin_uppercase(VM *vm, Value *args, int argc) {
    if (argc < 1 || args[0].type != VAL_STRING) return val_null();
    VMString *s = &args[0].as.string;
    char *data = arena_alloc(vm->arena, s->length + 1);
    for (uint32_t i = 0; i < s->length; i++) {
        char c = s->data[i];
        data[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    data[s->length] = '\0';
    return val_string(vm, data, s->length);
}

static Value builtin_lowercase(VM *vm, Value *args, int argc) {
    if (argc < 1 || args[0].type != VAL_STRING) return val_null();
    VMString *s = &args[0].as.string;
    char *data = arena_alloc(vm->arena, s->length + 1);
    for (uint32_t i = 0; i < s->length; i++) {
        char c = s->data[i];
        data[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    data[s->length] = '\0';
    return val_string(vm, data, s->length);
}

static Value builtin_trim(VM *vm, Value *args, int argc) {
    if (argc < 1 || args[0].type != VAL_STRING) return val_null();
    VMString *s = &args[0].as.string;
    const char *start = s->data;
    const char *end = s->data + s->length;
    while (start < end && (*start == ' ' || *start == '\t' ||
                           *start == '\n' || *start == '\r')) start++;
    while (end > start && (*(end-1) == ' ' || *(end-1) == '\t' ||
                           *(end-1) == '\n' || *(end-1) == '\r')) end--;
    return val_string(vm, start, (uint32_t)(end - start));
}

static Value builtin_length(VM *vm, Value *args, int argc) {
    (void)vm;
    if (argc < 1) return val_int(0);
    switch (args[0].type) {
        case VAL_STRING: return val_int(args[0].as.string.length);
        case VAL_ARRAY: return val_int(args[0].as.array->count);
        case VAL_OBJECT: return val_int(args[0].as.object->count);
        default: return val_int(0);
    }
}

static Value call_builtin(VM *vm, uint16_t builtin_idx, Value *args, int argc) {
    if (builtin_idx >= vm->module->builtin_count) {
        vm_error(vm, "Invalid builtin index");
        return val_null();
    }

    const char *name = vm->module->builtins[builtin_idx].name;

    if (strcmp(name, "uppercase") == 0) return builtin_uppercase(vm, args, argc);
    if (strcmp(name, "lowercase") == 0) return builtin_lowercase(vm, args, argc);
    if (strcmp(name, "trim") == 0) return builtin_trim(vm, args, argc);
    if (strcmp(name, "length") == 0) return builtin_length(vm, args, argc);

    vm_error(vm, "Unknown builtin: %s", name);
    return val_null();
}

/* Shared execution loop (full run or single-step) */
static VMResult vm_execute(VM *vm, bool single_step) {
    if (vm->frame_count == 0) {
        vm_error(vm, "No call frame");
        return VM_ERROR;
    }

    CallFrame *frame = &vm->frames[vm->frame_count - 1];

    #define BINARY_OP(op, result_type) do { \
        Value b = pop(vm); \
        Value a = pop(vm); \
        double da = (a.type == VAL_INT) ? (double)a.as.integer : a.as.number; \
        double db = (b.type == VAL_INT) ? (double)b.as.integer : b.as.number; \
        push(vm, result_type(da op db)); \
    } while (0)

    while (!vm->had_error) {
        vm->instructions_executed++;
        if (vm->step_hook_fn) {
            uint8_t chunk_kind = frame->kind == VM_FRAME_MAIN ? 0u : 1u;
            uint16_t chunk_index = frame->kind == VM_FRAME_MAIN ? 0u : frame->func_idx;
            uint32_t pc = (uint32_t)(frame->ip - frame->chunk->code);
            uint8_t opcode = frame->ip[0];
            uint32_t line = 0;
            uint32_t column = 0;
            const char *source_path = NULL;
            BytecodeDebugSpan span = {0};
            if (bytecode_find_debug_span(vm->module, chunk_kind, chunk_index, pc, &span)) {
                line = span.line;
                column = span.column;
            }
            if (chunk_kind == 1 &&
                vm->module &&
                vm->module->func_debug &&
                chunk_index < vm->module->func_count) {
                const FunctionDebugRef *debug_ref = &vm->module->func_debug[chunk_index];
                if (debug_ref->source_path && debug_ref->source_path_len > 0) {
                    source_path = debug_ref->source_path;
                }
            }
            vm->step_hook_fn(chunk_kind,
                             chunk_index,
                             pc,
                             opcode,
                             line,
                             column,
                             source_path,
                             vm->step_hook_userdata);
        }

        uint8_t instruction = read_byte(frame);

        switch (instruction) {
            case BC_NOP:
                break;

            case BC_CONST: {
                uint16_t idx = read_u16(frame);
                if (idx >= vm->module->const_count) {
                    vm_error(vm, "Invalid constant index");
                    break;
                }
                Constant *c = &vm->module->constants[idx];
                switch (c->type) {
                    case CONST_NULL: push(vm, val_null()); break;
                    case CONST_BOOL: push(vm, val_bool(c->v.boolean)); break;
                    case CONST_INT: push(vm, val_int(c->v.integer)); break;
                    case CONST_NUMBER: push(vm, val_number(c->v.number)); break;
                    case CONST_STRING:
                        push(vm, val_string(vm, c->v.string.data, c->v.string.length));
                        break;
                }
                break;
            }

            case BC_POP:
                pop(vm);
                break;

            case BC_DUP:
                push(vm, peek(vm, 0));
                break;

            case BC_LOAD: {
                uint16_t slot = read_u16(frame);
                push(vm, frame->slots[slot]);
                break;
            }

            case BC_STORE: {
                uint16_t slot = read_u16(frame);
                frame->slots[slot] = peek(vm, 0);
                break;
            }

            case BC_LOAD_GLOBAL: {
                uint16_t name_idx = read_u16(frame);
                const char *name = vm->module->strings[name_idx];
                Value *val = vm_get_global(vm, name);
                push(vm, val ? *val : val_null());
                break;
            }

            case BC_LOAD_FIELD: {
                uint16_t field_idx = read_u16(frame);
                const char *field = vm->module->strings[field_idx];
                Value obj = pop(vm);
                if (obj.type == VAL_OBJECT) {
                    Value *val = object_get(obj.as.object, field);
                    push(vm, val ? *val : val_null());
                } else {
                    push(vm, val_null());
                }
                break;
            }

            case BC_LOAD_INDEX: {
                Value index = pop(vm);
                Value arr = pop(vm);
                if (arr.type == VAL_ARRAY && index.type == VAL_INT) {
                    push(vm, array_get(arr.as.array, (uint32_t)index.as.integer));
                } else {
                    push(vm, val_null());
                }
                break;
            }

            case BC_STORE_FIELD: {
                uint16_t field_idx = read_u16(frame);
                const char *field = vm->module->strings[field_idx];
                Value obj = pop(vm);
                Value value = pop(vm);
                if (obj.type == VAL_OBJECT) {
                    object_set(vm, obj.as.object, field, value);
                } else {
                    vm_error(vm, "Cannot set field on non-object");
                }
                break;
            }

            case BC_STORE_INDEX: {
                Value index = pop(vm);
                Value arr = pop(vm);
                Value value = pop(vm);
                if (arr.type == VAL_ARRAY && index.type == VAL_INT) {
                    array_set(arr.as.array, (uint32_t)index.as.integer, value);
                } else {
                    vm_error(vm, "Cannot index into non-array");
                }
                break;
            }

            case BC_NULL:
                push(vm, val_null());
                break;

            case BC_TRUE:
                push(vm, val_bool(true));
                break;

            case BC_FALSE:
                push(vm, val_bool(false));
                break;

            case BC_INT: {
                int16_t val = read_i16(frame);
                push(vm, val_int(val));
                break;
            }

            case BC_ADD: {
                Value b = pop(vm);
                Value a = pop(vm);
                /* String concatenation */
                if (a.type == VAL_STRING && b.type == VAL_STRING) {
                    uint32_t len = a.as.string.length + b.as.string.length;
                    char *data = arena_alloc(vm->arena, len + 1);
                    memcpy(data, a.as.string.data, a.as.string.length);
                    memcpy(data + a.as.string.length, b.as.string.data, b.as.string.length);
                    data[len] = '\0';
                    push(vm, val_string(vm, data, len));
                }
                /* Integer addition */
                else if (a.type == VAL_INT && b.type == VAL_INT) {
                    push(vm, val_int(a.as.integer + b.as.integer));
                }
                /* Numeric addition */
                else {
                    double da = (a.type == VAL_INT) ? (double)a.as.integer : a.as.number;
                    double db = (b.type == VAL_INT) ? (double)b.as.integer : b.as.number;
                    push(vm, val_number(da + db));
                }
                break;
            }

            case BC_SUB: BINARY_OP(-, val_number); break;
            case BC_MUL: BINARY_OP(*, val_number); break;
            case BC_DIV: BINARY_OP(/, val_number); break;

            case BC_MOD: {
                Value b = pop(vm);
                Value a = pop(vm);
                if (a.type == VAL_INT && b.type == VAL_INT && b.as.integer != 0) {
                    push(vm, val_int(a.as.integer % b.as.integer));
                } else {
                    push(vm, val_null());
                }
                break;
            }

            case BC_NEG: {
                Value a = pop(vm);
                if (a.type == VAL_INT) {
                    push(vm, val_int(-a.as.integer));
                } else if (a.type == VAL_NUMBER) {
                    push(vm, val_number(-a.as.number));
                } else {
                    push(vm, val_null());
                }
                break;
            }

            case BC_EQ: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_equals(&a, &b)));
                break;
            }

            case BC_NEQ: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(!val_equals(&a, &b)));
                break;
            }

            case BC_LT: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_compare(&a, &b) < 0));
                break;
            }

            case BC_LTE: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_compare(&a, &b) <= 0));
                break;
            }

            case BC_GT: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_compare(&a, &b) > 0));
                break;
            }

            case BC_GTE: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_compare(&a, &b) >= 0));
                break;
            }

            case BC_AND: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_is_truthy(&a) && val_is_truthy(&b)));
                break;
            }

            case BC_OR: {
                Value b = pop(vm);
                Value a = pop(vm);
                push(vm, val_bool(val_is_truthy(&a) || val_is_truthy(&b)));
                break;
            }

            case BC_NOT: {
                Value a = pop(vm);
                push(vm, val_bool(!val_is_truthy(&a)));
                break;
            }

            case BC_JUMP: {
                int16_t offset = read_i16(frame);
                frame->ip += offset;
                break;
            }

            case BC_JUMP_IF_FALSE: {
                int16_t offset = read_i16(frame);
                Value cond = peek(vm, 0);
                if (!val_is_truthy(&cond)) {
                    frame->ip += offset;
                }
                break;
            }

            case BC_JUMP_IF_TRUE: {
                int16_t offset = read_i16(frame);
                Value cond = peek(vm, 0);
                if (val_is_truthy(&cond)) {
                    frame->ip += offset;
                }
                break;
            }

            case BC_ITER_START: {
                Value arr = pop(vm);
                if (arr.type != VAL_ARRAY) {
                    vm_error(vm, "Cannot iterate over non-array");
                    break;
                }
                VMIterator *iter = arena_alloc(vm->arena, sizeof(VMIterator));
                iter->array = arr.as.array->elements;
                iter->count = arr.as.array->count;
                iter->current = 0;
                Value iter_val;
                iter_val.type = VAL_ITERATOR;
                iter_val.as.iterator = iter;
                push(vm, iter_val);
                break;
            }

            case BC_ITER_NEXT: {
                int16_t offset = read_i16(frame);
                Value iter_val = peek(vm, 0);
                if (iter_val.type != VAL_ITERATOR) {
                    vm_error(vm, "Expected iterator");
                    break;
                }
                VMIterator *iter = iter_val.as.iterator;
                if (iter->current >= iter->count) {
                    frame->ip += offset;  /* Jump to end */
                } else {
                    /* Push current element */
                    push(vm, iter->array[iter->current++]);
                }
                break;
            }

            case BC_ITER_END:
                pop(vm);  /* Pop iterator */
                break;

            case BC_EMIT_LITERAL: {
                uint16_t idx = read_u16(frame);
                Constant *c = &vm->module->constants[idx];
                if (c->type == CONST_STRING) {
                    emit_output(vm, c->v.string.data, c->v.string.length);
                }
                break;
            }

            case BC_EMIT_TEXT: {
                Value val = pop(vm);
                Value str = val_to_string(vm, &val);
                if (str.type == VAL_STRING) {
                    emit_escaped(vm, str.as.string.data, str.as.string.length);
                }
                break;
            }

            case BC_EMIT_RAW: {
                Value val = pop(vm);
                Value str = val_to_string(vm, &val);
                if (str.type == VAL_STRING) {
                    emit_output(vm, str.as.string.data, str.as.string.length);
                }
                break;
            }

            case BC_EMIT_TAG_OPEN: {
                uint16_t name_idx = read_u16(frame);
                const char *tag = vm->module->strings[name_idx];
                emit_output(vm, "<", 1);
                emit_output(vm, tag, (uint32_t)strlen(tag));
                /* Don't emit > here - wait for BC_EMIT_TAG_END */
                break;
            }

            case BC_EMIT_TAG_END:
                emit_output(vm, ">", 1);
                break;

            case BC_EMIT_TAG_CLOSE: {
                uint16_t name_idx = read_u16(frame);
                const char *tag = vm->module->strings[name_idx];
                emit_output(vm, "</", 2);
                emit_output(vm, tag, (uint32_t)strlen(tag));
                emit_output(vm, ">", 1);
                break;
            }

            case BC_EMIT_TAG_SELF:
                emit_output(vm, "/>", 2);
                break;

            case BC_EMIT_ATTR_START: {
                uint16_t name_idx = read_u16(frame);
                const char *name = vm->module->strings[name_idx];
                emit_output(vm, " ", 1);
                emit_output(vm, name, (uint32_t)strlen(name));
                emit_output(vm, "=\"", 2);
                break;
            }

            case BC_EMIT_ATTR_END:
                emit_output(vm, "\"", 1);
                break;

            case BC_CALL_BUILTIN: {
                uint16_t builtin_idx = read_u16(frame);
                uint8_t argc = read_byte(frame);

                size_t stack_available = (size_t)(vm->stack_top - vm->stack);
                if ((size_t)argc > stack_available) {
                    vm_error(vm, "Stack underflow in builtin call (argc=%u, available=%zu)",
                             (unsigned)argc, stack_available);
                    break;
                }

                /* Collect arguments from stack */
                Value args[256];
                for (int i = argc - 1; i >= 0; i--) {
                    args[i] = pop(vm);
                }

                Value result = call_builtin(vm, builtin_idx, args, argc);
                push(vm, result);
                break;
            }

            case BC_CALL: {
                uint16_t func_idx = read_u16(frame);
                uint8_t argc = read_byte(frame);

                size_t stack_available = (size_t)(vm->stack_top - vm->stack);
                if ((size_t)argc > stack_available) {
                    vm_error(vm, "Stack underflow in call (argc=%u, available=%zu)",
                             (unsigned)argc, stack_available);
                    break;
                }

                Chunk *fn_chunk = bytecode_get_function(vm->module, func_idx);
                if (!fn_chunk) {
                    vm_error(vm, "Invalid function index: %u", (unsigned)func_idx);
                    break;
                }

                if (vm->frame_count >= VM_FRAMES_MAX) {
                    vm_error(vm, "Call stack overflow");
                    break;
                }

                CallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->chunk = fn_chunk;
                new_frame->ip = fn_chunk->code;
                new_frame->slots = vm->stack_top - argc;
                new_frame->kind = VM_FRAME_FUNCTION;
                new_frame->func_idx = func_idx;
                frame = new_frame;
                break;
            }

            case BC_CALL_PIPE: {
                uint16_t func_idx = read_u16(frame);
                uint8_t argc = 1;

                size_t stack_available = (size_t)(vm->stack_top - vm->stack);
                if ((size_t)argc > stack_available) {
                    vm_error(vm, "Stack underflow in pipe call (available=%zu)", stack_available);
                    break;
                }

                Chunk *fn_chunk = bytecode_get_function(vm->module, func_idx);
                if (!fn_chunk) {
                    vm_error(vm, "Invalid pipe function index: %u", (unsigned)func_idx);
                    break;
                }

                if (vm->frame_count >= VM_FRAMES_MAX) {
                    vm_error(vm, "Call stack overflow");
                    break;
                }

                CallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->chunk = fn_chunk;
                new_frame->ip = fn_chunk->code;
                new_frame->slots = vm->stack_top - argc;
                new_frame->kind = VM_FRAME_FUNCTION;
                new_frame->func_idx = func_idx;
                frame = new_frame;
                break;
            }

            case BC_ARRAY_NEW: {
                uint16_t count = read_u16(frame);
                Value arr = val_array(vm, count);
                /* Pop elements in reverse order */
                for (int i = count - 1; i >= 0; i--) {
                    arr.as.array->elements[i] = pop(vm);
                }
                arr.as.array->count = count;
                push(vm, arr);
                break;
            }

            case BC_OBJECT_NEW: {
                uint16_t count = read_u16(frame);
                Value obj = val_object(vm, count);
                push(vm, obj);
                break;
            }

            case BC_OBJECT_SET: {
                uint16_t key_idx = read_u16(frame);
                const char *key = vm->module->strings[key_idx];
                Value value = pop(vm);
                Value obj = peek(vm, 0);
                if (obj.type == VAL_OBJECT) {
                    object_set(vm, obj.as.object, key, value);
                }
                break;
            }

            case BC_CONCAT: {
                uint8_t count = read_byte(frame);
                if (count == 0) {
                    push(vm, val_string(vm, "", 0));
                    break;
                }

                Value parts[256];
                uint32_t total_len = 0;
                for (int i = count - 1; i >= 0; i--) {
                    Value v = pop(vm);
                    parts[i] = val_to_string(vm, &v);
                    if (parts[i].type == VAL_STRING) {
                        total_len += parts[i].as.string.length;
                    }
                }

                char *out = arena_alloc(vm->arena, total_len + 1);
                uint32_t pos = 0;
                for (uint8_t i = 0; i < count; i++) {
                    if (parts[i].type != VAL_STRING) continue;
                    memcpy(out + pos, parts[i].as.string.data, parts[i].as.string.length);
                    pos += parts[i].as.string.length;
                }
                out[pos] = '\0';
                push(vm, val_string(vm, out, pos));
                break;
            }

            case BC_FETCH_DATA: {
                uint16_t req_idx = read_u16(frame);
                if (req_idx >= vm->module->data_req_count) {
                    vm_error(vm, "Invalid data requirement index: %d", req_idx);
                    push(vm, val_null());
                    break;
                }

                DataRequirement *req = &vm->module->data_reqs[req_idx];

                if (vm->fetch_fn) {
                    /* Call fetch callback with request metadata + current frame slots. */
                    Value result = vm->fetch_fn(req, frame->slots, vm->fetch_userdata);
                    if (vm->awaiting) {
                        /* Re-run this opcode after host provides data. */
                        frame->ip -= 3;
                        return VM_AWAIT_DATA;
                    }
                    push(vm, result);
                } else {
                    /* No fetch callback - check globals */
                    Value *global = vm_get_global(vm, req->name);
                    if (global) {
                        push(vm, *global);
                    } else {
                        push(vm, val_null());
                    }
                }
                break;
            }

            case BC_FETCH_WAIT: {
                /* For async data fetching - currently no-op */
                break;
            }

            case BC_COMPONENT_START: {
                uint16_t func_idx = read_u16(frame);
                Chunk *comp_chunk = bytecode_get_function(vm->module, func_idx);
                if (!comp_chunk) {
                    vm_error(vm, "Invalid component function index: %d", func_idx);
                    break;
                }

                /* Children value is on top, props below it */
                Value children = pop(vm);
                Value props = pop(vm);

                /* Push new call frame for component */
                if (vm->frame_count >= VM_FRAMES_MAX) {
                    vm_error(vm, "Component call stack overflow");
                    break;
                }

                CallFrame *new_frame = &vm->frames[vm->frame_count++];
                new_frame->chunk = comp_chunk;
                new_frame->ip = comp_chunk->code;
                new_frame->slots = vm->stack_top;
                new_frame->kind = VM_FRAME_COMPONENT;
                new_frame->func_idx = func_idx;

                if (vm->component_start_fn) {
                    vm->component_start_fn(func_idx, vm->component_boundary_userdata);
                }

                /* Push props as first argument (slot 0) */
                push(vm, props);

                /* Push children as second argument (slot 1) */
                push(vm, children);

                /* Update frame pointer for loop */
                frame = new_frame;
                break;
            }

            case BC_COMPONENT_END: {
                /* Component end is handled by BC_RETURN in the component */
                break;
            }

            case BC_COMPONENT_LOAD: {
                uint16_t ref_idx = read_u16(frame);
                uint8_t argc = read_byte(frame);
                VMComponentLoadFn load_fn = vm->component_fn;
                void *load_userdata = vm->component_userdata;

                if (ref_idx >= vm->module->comp_ref_count) {
                    vm_error(vm, "Invalid component ref index: %u", (unsigned)ref_idx);
                    break;
                }

                size_t stack_available = (size_t)(vm->stack_top - vm->stack);
                if ((size_t)argc > stack_available) {
                    vm_error(vm, "Stack underflow in component load (argc=%u, available=%zu)",
                             (unsigned)argc, stack_available);
                    break;
                }

                Value args[256];
                for (uint8_t i = 0; i < argc; i++) {
                    args[i] = vm->stack_top[-(int)argc + (int)i];
                }

                const ComponentRef *ref = &vm->module->comp_refs[ref_idx];
                if (!load_fn) {
                    vm_error(vm, "No component loader for dynamic component: %s",
                             ref->name ? ref->name : "<unknown>");
                    break;
                }

                Value rendered = load_fn(ref, args, argc, load_userdata);
                if (vm->awaiting) {
                    /* Re-run this opcode once component HTML is available. */
                    frame->ip -= 4;
                    return VM_AWAIT_DATA;
                }

                vm->stack_top -= argc;

                Value out = val_to_string(vm, &rendered);
                if (out.type == VAL_STRING && out.as.string.length > 0) {
                    emit_output(vm, out.as.string.data, out.as.string.length);
                }
                break;
            }

            case BC_COMPONENT_LINKED: {
                uint16_t ref_idx = read_u16(frame);
                uint8_t argc = read_byte(frame);

                if (ref_idx >= vm->module->comp_ref_count) {
                    vm_error(vm, "Invalid component ref index: %u", (unsigned)ref_idx);
                    break;
                }

                size_t stack_available = (size_t)(vm->stack_top - vm->stack);
                if ((size_t)argc > stack_available) {
                    vm_error(vm, "Stack underflow in component load (argc=%u, available=%zu)",
                             (unsigned)argc, stack_available);
                    break;
                }

                const ComponentRef *ref = &vm->module->comp_refs[ref_idx];

                /* Linked fast-path: execute prelinked function directly in this VM. */
                if (vm->linked_component_func_targets &&
                    vm->linked_component_func_targets[ref_idx] != VM_LINKED_NONE) {
                    uint16_t func_idx = vm->linked_component_func_targets[ref_idx];
                    Chunk *comp_chunk = bytecode_get_function(vm->module, func_idx);
                    if (!comp_chunk) {
                        vm_error(vm, "Invalid linked component function index: %u", (unsigned)func_idx);
                        break;
                    }
                    if (vm->frame_count >= VM_FRAMES_MAX) {
                        vm_error(vm, "Component call stack overflow");
                        break;
                    }
                    CallFrame *new_frame = &vm->frames[vm->frame_count++];
                    new_frame->chunk = comp_chunk;
                    new_frame->ip = comp_chunk->code;
                    new_frame->slots = vm->stack_top - argc;
                    new_frame->kind = VM_FRAME_COMPONENT;
                    new_frame->func_idx = func_idx;
                    if (vm->component_start_fn) {
                        vm->component_start_fn(func_idx, vm->component_boundary_userdata);
                    }
                    frame = new_frame;
                    break;
                }

                /* Fallback: use linked component loader callback if available */
                if (vm->linked_component_fn) {
                    Value args[256];
                    for (uint8_t i = 0; i < argc; i++) {
                        args[i] = vm->stack_top[-(int)argc + (int)i];
                    }

                    Value rendered = vm->linked_component_fn(ref, args, argc, vm->linked_component_userdata);
                    if (vm->awaiting) {
                        frame->ip -= 4;
                        return VM_AWAIT_DATA;
                    }

                    vm->stack_top -= argc;

                    Value out = val_to_string(vm, &rendered);
                    if (out.type == VAL_STRING && out.as.string.length > 0) {
                        emit_output(vm, out.as.string.data, out.as.string.length);
                    }
                    break;
                }

                /* No fallback available - error out */
                vm_error(vm, "Linked component not resolved and no loader available: %s",
                         ref->path ? ref->path : (ref->name ? ref->name : "<unknown>"));
                break;
            }

            case BC_SLOT_START: {
                uint16_t slot_idx = read_u16(frame);
                (void)slot_idx;
                if (vm->capture_depth >= VM_CAPTURE_MAX) {
                    vm_error(vm, "Slot capture stack overflow");
                    break;
                }
                vm->captures[vm->capture_depth].data = NULL;
                vm->captures[vm->capture_depth].len = 0;
                vm->captures[vm->capture_depth].cap = 0;
                vm->capture_depth++;
                break;
            }

            case BC_SLOT_END: {
                if (vm->capture_depth <= 0) {
                    vm_error(vm, "Slot end without slot start");
                    break;
                }
                vm->capture_depth--;
                VMCaptureBuffer *cap = &vm->captures[vm->capture_depth];
                if (!cap->data || cap->len == 0) {
                    push(vm, val_string(vm, "", 0));
                } else {
                    push(vm, val_string(vm, cap->data, cap->len));
                }
                break;
            }

            case BC_SLOT_DEFAULT: {
                Value children = pop(vm);
                if (frame->kind == VM_FRAME_COMPONENT && vm->slot_default_start_fn) {
                    vm->slot_default_start_fn(frame->func_idx, vm->component_boundary_userdata);
                }
                if (children.type != VAL_NULL) {
                    Value out = val_to_string(vm, &children);
                    if (out.type == VAL_STRING && out.as.string.length > 0) {
                        emit_output(vm, out.as.string.data, out.as.string.length);
                    }
                }
                if (frame->kind == VM_FRAME_COMPONENT && vm->slot_default_end_fn) {
                    vm->slot_default_end_fn(frame->func_idx, vm->component_boundary_userdata);
                }
                break;
            }

            case BC_DEP_START: {
                /* Start dependency tracking region */
                uint16_t dep_idx = read_u16(frame);
                (void)dep_idx;  /* Available for runtime dependency tracking */
                /* TODO: Record which dependencies are active for this output region */
                break;
            }

            case BC_DEP_END: {
                /* End dependency tracking region */
                /* TODO: Store association between output and dependencies */
                break;
            }

            case BC_RETURN: {
                VMFrameKind returning_kind = frame->kind;
                uint16_t returning_func_idx = frame->func_idx;
                /* Restore caller stack base and pop frame. */
                vm->stack_top = frame->slots;
                vm->frame_count--;
                if (returning_kind == VM_FRAME_COMPONENT && vm->component_end_fn) {
                    vm->component_end_fn(returning_func_idx, vm->component_boundary_userdata);
                }
                if (vm->frame_count == 0) {
                    return VM_OK;
                }
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }

            case BC_MUTATE_START: {
                uint16_t idx = read_u16(frame);
                if (idx >= vm->module->mut_req_count) {
                    vm_error(vm, "Invalid mutation requirement index: %d", idx);
                    break;
                }
                MutationRequirement *req = &vm->module->mut_reqs[idx];
                const char *type_str = req->type == MUTATE_INSERT ? "insert" :
                                       req->type == MUTATE_UPDATE ? "update" : "delete";
                char buf[512];
                int n = snprintf(buf, sizeof(buf),
                    "<form method=\"post\" data-mot-mutation=\"%s\" data-mot-type=\"%s\">",
                    req->target, type_str);
                if (n > 0 && (size_t)n < sizeof(buf)) {
                    emit_output(vm, buf, (uint32_t)n);
                }
                break;
            }

            case BC_MUTATE_END:
                emit_output(vm, "</form>", 7);
                break;

            case BC_HALT:
                return vm->had_error ? VM_ERROR : VM_OK;

            default:
                vm_error(vm, "Unknown opcode: %d", instruction);
                break;
        }

        if (single_step) {
            return vm->had_error ? VM_ERROR : VM_OK;
        }
    }

    #undef BINARY_OP

    return VM_ERROR;
}

/* Main execution loop */
VMResult vm_run(VM *vm) {
    return vm_execute(vm, false);
}

/* Execute a single instruction */
VMResult vm_step(VM *vm) {
    return vm_execute(vm, true);
}
