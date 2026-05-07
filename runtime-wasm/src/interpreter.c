/*
 * WASM Bytecode Interpreter for Motus
 *
 * Minimal interpreter designed for edge execution.
 * Compiled to WASM using Emscripten or clang.
 */

#include "host.h"
#include "stream.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Bytecode magic and opcodes (must match compiler/bytecode.h) */
#define BYTECODE_MAGIC 0x00544F4D

/* Opcodes - subset needed for interpreter */
typedef enum {
    BC_NOP = 0,
    BC_CONST,
    BC_POP,
    BC_DUP,
    BC_LOAD,
    BC_STORE,
    BC_LOAD_GLOBAL,
    BC_LOAD_FIELD,
    BC_LOAD_INDEX,
    BC_STORE_FIELD,
    BC_STORE_INDEX,
    BC_NULL,
    BC_TRUE,
    BC_FALSE,
    BC_INT,
    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_MOD,
    BC_NEG,
    BC_EQ,
    BC_NEQ,
    BC_LT,
    BC_LTE,
    BC_GT,
    BC_GTE,
    BC_AND,
    BC_OR,
    BC_NOT,
    BC_JUMP,
    BC_JUMP_IF_FALSE,
    BC_JUMP_IF_TRUE,
    BC_ITER_START,
    BC_ITER_NEXT,
    BC_ITER_END,
    BC_EMIT_LITERAL,
    BC_EMIT_TEXT,
    BC_EMIT_RAW,
    BC_EMIT_ATTR_START,
    BC_EMIT_ATTR_END,
    BC_EMIT_TAG_OPEN,
    BC_EMIT_TAG_CLOSE,
    BC_EMIT_TAG_SELF,
    BC_FETCH_DATA,
    BC_FETCH_WAIT,
    BC_CALL,
    BC_CALL_BUILTIN,
    BC_CALL_PIPE,
    BC_RETURN,
    BC_COMPONENT_START,
    BC_COMPONENT_END,
    BC_SLOT_START,
    BC_SLOT_END,
    BC_SLOT_DEFAULT,
    BC_DEP_START,
    BC_DEP_END,
    BC_ARRAY_NEW,
    BC_OBJECT_NEW,
    BC_OBJECT_SET,
    BC_CONCAT,
    BC_HALT,
} OpCode;

/* Value types */
typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_ARRAY,
    VAL_OBJECT,
    VAL_ITERATOR,
} ValueType;

/* Simple value representation */
typedef struct Value Value;

typedef struct {
    Value *items;
    uint32_t count;
    uint32_t cap;
} Array;

typedef struct {
    char **keys;
    Value *values;
    uint32_t count;
    uint32_t cap;
} Object;

typedef struct {
    Value *array;
    uint32_t index;
    uint32_t count;
} Iterator;

struct Value {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *data;
            uint32_t len;
        } string;
        Array *array;
        Object *object;
        Iterator iterator;
    } as;
};

/* Constant types */
typedef enum {
    CONST_NULL,
    CONST_BOOL,
    CONST_INT,
    CONST_NUMBER,
    CONST_STRING,
} ConstType;

/* Runtime state */
#define STACK_MAX 256
#define FRAMES_MAX 64
#define LOCALS_MAX 256

typedef struct {
    uint8_t *ip;           /* Instruction pointer */
    Value *slots;          /* Local variable base */
    uint32_t slot_count;
} CallFrame;

typedef struct {
    /* Bytecode data */
    const uint8_t *bytecode;
    uint32_t bytecode_len;

    /* Parsed bytecode sections */
    const uint8_t *constants;
    uint32_t const_count;
    const uint8_t *strings;
    uint32_t string_count;
    const uint8_t *data_reqs;
    uint32_t data_req_count;
    const uint8_t *deps;
    uint32_t dep_count;
    const uint8_t *code;
    uint32_t code_len;

    /* Value stack */
    Value stack[STACK_MAX];
    int stack_top;

    /* Call frames */
    CallFrame frames[FRAMES_MAX];
    int frame_count;

    /* Local variables */
    Value locals[LOCALS_MAX];

    /* Output stream */
    StreamBuffer stream;

    /* State */
    int state;
    bool had_error;

    /* Pending data request */
    uint32_t pending_req_id;
} Runtime;

static Runtime runtime;

/* Helper: read u16 from bytecode */
static uint16_t read_u16(uint8_t **ip) {
    uint16_t val = (*ip)[0] | ((*ip)[1] << 8);
    *ip += 2;
    return val;
}

/* Helper: read i16 from bytecode */
static int16_t read_i16(uint8_t **ip) {
    return (int16_t)read_u16(ip);
}

/* Helper: get string from string table */
static const char *get_string(uint16_t idx) {
    const uint8_t *p = runtime.strings;
    for (uint32_t i = 0; i < idx && i < runtime.string_count; i++) {
        uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        p += 4 + len;
    }
    if (idx >= runtime.string_count) return "";
    uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    (void)len;
    return (const char *)(p + 4);
}

/* Helper: get constant from constant pool */
static Value get_constant(uint16_t idx) {
    Value val = { .type = VAL_NULL };
    const uint8_t *p = runtime.constants;

    for (uint32_t i = 0; i < idx && i < runtime.const_count; i++) {
        uint8_t type = *p++;
        switch (type) {
            case CONST_NULL: break;
            case CONST_BOOL: p++; break;
            case CONST_INT: p += 8; break;
            case CONST_NUMBER: p += 8; break;
            case CONST_STRING: {
                uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
                p += 4 + len;
                break;
            }
        }
    }

    if (idx >= runtime.const_count) return val;

    uint8_t type = *p++;
    switch (type) {
        case CONST_NULL:
            val.type = VAL_NULL;
            break;
        case CONST_BOOL:
            val.type = VAL_BOOL;
            val.as.boolean = *p != 0;
            break;
        case CONST_INT:
            val.type = VAL_INT;
            memcpy(&val.as.integer, p, 8);
            break;
        case CONST_NUMBER:
            val.type = VAL_FLOAT;
            memcpy(&val.as.number, p, 8);
            break;
        case CONST_STRING: {
            uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
            val.type = VAL_STRING;
            val.as.string.data = (char *)(p + 4);
            val.as.string.len = len;
            break;
        }
    }

    return val;
}

/* Helper: get dependency path */
static const char *get_dep_path(uint16_t idx) {
    const uint8_t *p = runtime.deps;
    for (uint32_t i = 0; i < idx && i < runtime.dep_count; i++) {
        uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        p += 4 + len;
    }
    if (idx >= runtime.dep_count) return "";
    uint32_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    (void)len;
    return (const char *)(p + 4);
}

/* Stack operations */
static void push(Value val) {
    if (runtime.stack_top >= STACK_MAX) {
        host_error("Stack overflow", 14);
        runtime.had_error = true;
        return;
    }
    runtime.stack[runtime.stack_top++] = val;
}

static Value pop(void) {
    if (runtime.stack_top <= 0) {
        host_error("Stack underflow", 15);
        runtime.had_error = true;
        return (Value){ .type = VAL_NULL };
    }
    return runtime.stack[--runtime.stack_top];
}

static Value peek(int distance) {
    if (runtime.stack_top - 1 - distance < 0) {
        return (Value){ .type = VAL_NULL };
    }
    return runtime.stack[runtime.stack_top - 1 - distance];
}

/* Value helpers */
static bool is_truthy(Value val) {
    switch (val.type) {
        case VAL_NULL: return false;
        case VAL_BOOL: return val.as.boolean;
        case VAL_INT: return val.as.integer != 0;
        case VAL_FLOAT: return val.as.number != 0.0;
        case VAL_STRING: return val.as.string.len > 0;
        case VAL_ARRAY: return val.as.array && val.as.array->count > 0;
        case VAL_OBJECT: return val.as.object && val.as.object->count > 0;
        default: return true;
    }
}

static void value_to_string(Value val, StreamBuffer *stream) {
    switch (val.type) {
        case VAL_NULL:
            break;
        case VAL_BOOL:
            stream_write_str(stream, val.as.boolean ? "true" : "false");
            break;
        case VAL_INT:
            stream_write_int(stream, val.as.integer);
            break;
        case VAL_FLOAT:
            stream_write_float(stream, val.as.number);
            break;
        case VAL_STRING:
            stream_write_text(stream, val.as.string.data, val.as.string.len);
            break;
        case VAL_ARRAY:
            stream_write_str(stream, "[Array]");
            break;
        case VAL_OBJECT:
            stream_write_str(stream, "[Object]");
            break;
        default:
            break;
    }
}

/* Parse bytecode header and sections */
static int parse_bytecode(const uint8_t *data, uint32_t len) {
    if (len < 16) {
        host_error("Bytecode too short", 17);
        return MOT_ERROR;
    }

    /* Check magic */
    uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    if (magic != BYTECODE_MAGIC) {
        host_error("Invalid bytecode magic", 21);
        return MOT_ERROR;
    }

    /* Read header */
    const uint8_t *p = data + 8;  /* Skip magic + version */

    /* Read section counts */
    runtime.const_count = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    p += 4;
    runtime.string_count = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    p += 4;
    runtime.data_req_count = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    p += 4;
    runtime.dep_count = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    p += 4;

    /* Skip builtins count and func count */
    p += 8;

    /* Code length */
    runtime.code_len = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    p += 4;

    /* Section data follows header */
    runtime.constants = p;
    /* Skip constants */
    for (uint32_t i = 0; i < runtime.const_count; i++) {
        uint8_t type = *p++;
        switch (type) {
            case CONST_NULL: break;
            case CONST_BOOL: p++; break;
            case CONST_INT: p += 8; break;
            case CONST_NUMBER: p += 8; break;
            case CONST_STRING: {
                uint32_t slen = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
                p += 4 + slen;
                break;
            }
        }
    }

    runtime.strings = p;
    /* Skip strings */
    for (uint32_t i = 0; i < runtime.string_count; i++) {
        uint32_t slen = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        p += 4 + slen;
    }

    runtime.data_reqs = p;
    /* Skip data reqs (simplified - would need proper parsing) */

    runtime.deps = p;
    /* Skip deps */
    for (uint32_t i = 0; i < runtime.dep_count; i++) {
        uint32_t dlen = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
        p += 4 + dlen;
    }

    /* Code section */
    runtime.code = p;

    return MOT_OK;
}

/* Execute bytecode */
static int execute(void) {
    CallFrame *frame = &runtime.frames[runtime.frame_count - 1];

    while (!runtime.had_error) {
        uint8_t op = *frame->ip++;

        switch (op) {
            case BC_NOP:
                break;

            case BC_CONST: {
                uint16_t idx = read_u16(&frame->ip);
                push(get_constant(idx));
                break;
            }

            case BC_POP:
                pop();
                break;

            case BC_DUP:
                push(peek(0));
                break;

            case BC_LOAD: {
                uint16_t slot = read_u16(&frame->ip);
                push(frame->slots[slot]);
                break;
            }

            case BC_STORE: {
                uint16_t slot = read_u16(&frame->ip);
                frame->slots[slot] = pop();
                break;
            }

            case BC_NULL:
                push((Value){ .type = VAL_NULL });
                break;

            case BC_TRUE:
                push((Value){ .type = VAL_BOOL, .as.boolean = true });
                break;

            case BC_FALSE:
                push((Value){ .type = VAL_BOOL, .as.boolean = false });
                break;

            case BC_INT: {
                int16_t val = read_i16(&frame->ip);
                push((Value){ .type = VAL_INT, .as.integer = val });
                break;
            }

            case BC_ADD: {
                Value b = pop();
                Value a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push((Value){ .type = VAL_INT, .as.integer = a.as.integer + b.as.integer });
                } else if (a.type == VAL_FLOAT || b.type == VAL_FLOAT) {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    push((Value){ .type = VAL_FLOAT, .as.number = av + bv });
                } else {
                    push((Value){ .type = VAL_NULL });
                }
                break;
            }

            case BC_SUB: {
                Value b = pop();
                Value a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push((Value){ .type = VAL_INT, .as.integer = a.as.integer - b.as.integer });
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    push((Value){ .type = VAL_FLOAT, .as.number = av - bv });
                }
                break;
            }

            case BC_MUL: {
                Value b = pop();
                Value a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    push((Value){ .type = VAL_INT, .as.integer = a.as.integer * b.as.integer });
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    push((Value){ .type = VAL_FLOAT, .as.number = av * bv });
                }
                break;
            }

            case BC_DIV: {
                Value b = pop();
                Value a = pop();
                double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                if (bv == 0) {
                    host_error("Division by zero", 16);
                    runtime.had_error = true;
                } else {
                    push((Value){ .type = VAL_FLOAT, .as.number = av / bv });
                }
                break;
            }

            case BC_MOD: {
                Value b = pop();
                Value a = pop();
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    if (b.as.integer == 0) {
                        host_error("Modulo by zero", 14);
                        runtime.had_error = true;
                    } else {
                        push((Value){ .type = VAL_INT, .as.integer = a.as.integer % b.as.integer });
                    }
                } else {
                    push((Value){ .type = VAL_NULL });
                }
                break;
            }

            case BC_NEG: {
                Value a = pop();
                if (a.type == VAL_INT) {
                    push((Value){ .type = VAL_INT, .as.integer = -a.as.integer });
                } else if (a.type == VAL_FLOAT) {
                    push((Value){ .type = VAL_FLOAT, .as.number = -a.as.number });
                } else {
                    push((Value){ .type = VAL_NULL });
                }
                break;
            }

            case BC_EQ: {
                Value b = pop();
                Value a = pop();
                bool eq = false;
                if (a.type == b.type) {
                    switch (a.type) {
                        case VAL_NULL: eq = true; break;
                        case VAL_BOOL: eq = a.as.boolean == b.as.boolean; break;
                        case VAL_INT: eq = a.as.integer == b.as.integer; break;
                        case VAL_FLOAT: eq = a.as.number == b.as.number; break;
                        case VAL_STRING:
                            eq = a.as.string.len == b.as.string.len &&
                                 memcmp(a.as.string.data, b.as.string.data, a.as.string.len) == 0;
                            break;
                        default: eq = false;
                    }
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = eq });
                break;
            }

            case BC_NEQ: {
                Value b = pop();
                Value a = pop();
                bool eq = false;
                if (a.type == b.type) {
                    switch (a.type) {
                        case VAL_NULL: eq = true; break;
                        case VAL_BOOL: eq = a.as.boolean == b.as.boolean; break;
                        case VAL_INT: eq = a.as.integer == b.as.integer; break;
                        case VAL_FLOAT: eq = a.as.number == b.as.number; break;
                        default: eq = false;
                    }
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = !eq });
                break;
            }

            case BC_LT: {
                Value b = pop();
                Value a = pop();
                bool result = false;
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    result = a.as.integer < b.as.integer;
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    result = av < bv;
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = result });
                break;
            }

            case BC_LTE: {
                Value b = pop();
                Value a = pop();
                bool result = false;
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    result = a.as.integer <= b.as.integer;
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    result = av <= bv;
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = result });
                break;
            }

            case BC_GT: {
                Value b = pop();
                Value a = pop();
                bool result = false;
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    result = a.as.integer > b.as.integer;
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    result = av > bv;
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = result });
                break;
            }

            case BC_GTE: {
                Value b = pop();
                Value a = pop();
                bool result = false;
                if (a.type == VAL_INT && b.type == VAL_INT) {
                    result = a.as.integer >= b.as.integer;
                } else {
                    double av = a.type == VAL_FLOAT ? a.as.number : (double)a.as.integer;
                    double bv = b.type == VAL_FLOAT ? b.as.number : (double)b.as.integer;
                    result = av >= bv;
                }
                push((Value){ .type = VAL_BOOL, .as.boolean = result });
                break;
            }

            case BC_NOT: {
                Value a = pop();
                push((Value){ .type = VAL_BOOL, .as.boolean = !is_truthy(a) });
                break;
            }

            case BC_JUMP: {
                int16_t offset = read_i16(&frame->ip);
                frame->ip += offset;
                break;
            }

            case BC_JUMP_IF_FALSE: {
                int16_t offset = read_i16(&frame->ip);
                if (!is_truthy(peek(0))) {
                    frame->ip += offset;
                }
                break;
            }

            case BC_JUMP_IF_TRUE: {
                int16_t offset = read_i16(&frame->ip);
                if (is_truthy(peek(0))) {
                    frame->ip += offset;
                }
                break;
            }

            case BC_ITER_START: {
                Value arr = pop();
                Value iter = { .type = VAL_ITERATOR };
                if (arr.type == VAL_ARRAY && arr.as.array) {
                    iter.as.iterator.array = arr.as.array->items;
                    iter.as.iterator.index = 0;
                    iter.as.iterator.count = arr.as.array->count;
                } else {
                    iter.as.iterator.array = NULL;
                    iter.as.iterator.index = 0;
                    iter.as.iterator.count = 0;
                }
                push(iter);
                break;
            }

            case BC_ITER_NEXT: {
                int16_t offset = read_i16(&frame->ip);
                Value *iter = &runtime.stack[runtime.stack_top - 1];
                if (iter->type == VAL_ITERATOR &&
                    iter->as.iterator.index < iter->as.iterator.count) {
                    /* Push current item */
                    push(iter->as.iterator.array[iter->as.iterator.index]);
                    iter->as.iterator.index++;
                } else {
                    /* End of iteration - jump */
                    frame->ip += offset;
                }
                break;
            }

            case BC_ITER_END:
                pop();  /* Pop iterator */
                break;

            case BC_EMIT_LITERAL: {
                uint16_t idx = read_u16(&frame->ip);
                const char *str = get_string(idx);
                stream_write_str(&runtime.stream, str);
                break;
            }

            case BC_EMIT_TEXT: {
                Value val = pop();
                value_to_string(val, &runtime.stream);
                break;
            }

            case BC_EMIT_RAW: {
                Value val = pop();
                if (val.type == VAL_STRING) {
                    stream_write_raw(&runtime.stream, val.as.string.data, val.as.string.len);
                }
                break;
            }

            case BC_DEP_START: {
                uint16_t idx = read_u16(&frame->ip);
                const char *path = get_dep_path(idx);
                host_dep_start(path, (uint32_t)strlen(path));
                break;
            }

            case BC_DEP_END:
                host_dep_end();
                break;

            case BC_FETCH_DATA: {
                uint16_t idx = read_u16(&frame->ip);
                (void)idx;
                /* Would trigger async data fetch */
                /* For now, push null */
                push((Value){ .type = VAL_NULL });
                break;
            }

            case BC_CALL_BUILTIN: {
                uint16_t idx = read_u16(&frame->ip);
                uint8_t argc = *frame->ip++;
                (void)idx;
                (void)argc;
                /* Simplified: just pop args and push null */
                for (int i = 0; i < argc; i++) pop();
                push((Value){ .type = VAL_NULL });
                break;
            }

            case BC_HALT:
                stream_flush(&runtime.stream);
                host_render_complete();
                runtime.state = MOT_STATE_DONE;
                return MOT_OK;

            default:
                /* Skip unknown opcodes with their operands */
                host_log("Unknown opcode", 14);
                break;
        }
    }

    return runtime.had_error ? MOT_ERROR : MOT_OK;
}

/*
 * Exported API
 */

WASM_EXPORT int mot_init(const uint8_t *bytecode, uint32_t len) {
    memset(&runtime, 0, sizeof(runtime));
    runtime.bytecode = bytecode;
    runtime.bytecode_len = len;
    runtime.state = MOT_STATE_IDLE;

    stream_init(&runtime.stream);

    int result = parse_bytecode(bytecode, len);
    if (result != MOT_OK) {
        runtime.state = MOT_STATE_ERROR;
        return result;
    }

    /* Set up initial call frame */
    runtime.frame_count = 1;
    runtime.frames[0].ip = (uint8_t *)runtime.code;
    runtime.frames[0].slots = runtime.locals;
    runtime.frames[0].slot_count = 0;

    return MOT_OK;
}

WASM_EXPORT int mot_render(void) {
    if (runtime.state == MOT_STATE_ERROR) {
        return MOT_ERROR;
    }

    runtime.state = MOT_STATE_RUNNING;
    return execute();
}

WASM_EXPORT int mot_resume(uint32_t req_id, const char *json_data, uint32_t len) {
    (void)req_id;
    (void)json_data;
    (void)len;
    /* Resume after data fetch - simplified for now */
    return mot_render();
}

WASM_EXPORT void mot_invalidate(const char *dep_path, uint32_t len) {
    (void)dep_path;
    (void)len;
    /* Mark cached regions as invalid */
}

WASM_EXPORT int mot_state(void) {
    return runtime.state;
}

WASM_EXPORT void mot_free(void) {
    memset(&runtime, 0, sizeof(runtime));
}

/*
 * Stub host functions for native testing
 */
#ifndef __EMSCRIPTEN__

void host_output(const char *data, uint32_t len) {
    fwrite(data, 1, len, stdout);
}

void host_output_text(const char *data, uint32_t len) {
    fwrite(data, 1, len, stdout);
}

void host_fetch_data(uint32_t req_id, uint32_t query_ref, uint32_t signature,
                     const char *name, const char *params_json,
                     uint32_t is_single) {
    (void)req_id;
    (void)query_ref;
    (void)signature;
    (void)name;
    (void)params_json;
    (void)is_single;
}

void host_error(const char *msg, uint32_t len) {
    fprintf(stderr, "Error: %.*s\n", (int)len, msg);
}

void host_log(const char *msg, uint32_t len) {
    fprintf(stderr, "Log: %.*s\n", (int)len, msg);
}

void host_render_complete(void) {
    printf("\n[Render complete]\n");
}

void host_dep_start(const char *path, uint32_t len) {
    (void)path;
    (void)len;
}

void host_dep_end(void) {
}

/* Simple test main for native builds */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <bytecode-file>\n", argv[0]);
        fprintf(stderr, "\nMotus WASM Runtime (native test build)\n");
        fprintf(stderr, "This is a test build of the WASM interpreter.\n");
        fprintf(stderr, "Compile with Emscripten for actual WASM output.\n");
        return 1;
    }

    /* Read bytecode file */
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *bytecode = malloc((size_t)size);
    if (!bytecode) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(f);
        return 1;
    }

    fread(bytecode, 1, (size_t)size, f);
    fclose(f);

    /* Initialize and run */
    int result = mot_init(bytecode, (uint32_t)size);
    if (result != MOT_OK) {
        fprintf(stderr, "Error: Failed to initialize runtime\n");
        free(bytecode);
        return 1;
    }

    result = mot_render();
    if (result != MOT_OK) {
        fprintf(stderr, "Error: Render failed\n");
    }

    mot_free();
    free(bytecode);
    return result == MOT_OK ? 0 : 1;
}

#endif
