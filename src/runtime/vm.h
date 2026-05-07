/*
 * Motus Virtual Machine
 *
 * Stack-based bytecode interpreter with streaming HTML output.
 */

#ifndef MOT_VM_H
#define MOT_VM_H

#include <stdint.h>
#include <stdbool.h>
#include "../util/arena.h"
#include "../compiler/bytecode.h"

/* Maximum stack size */
#define VM_STACK_MAX 256

/* Maximum call frames */
#define VM_FRAMES_MAX 64

/* Maximum nested slot-capture regions */
#define VM_CAPTURE_MAX 16

/* Runtime value types */
typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_NUMBER,
    VAL_STRING,
    VAL_ARRAY,
    VAL_OBJECT,
    VAL_ITERATOR,   /* For-loop iteration state */
} ValueType;

/* Forward declarations for types used by pointer only */
typedef struct VMArray VMArray;
typedef struct VMObject VMObject;
typedef struct VMIterator VMIterator;

/* String value (reference counted or arena allocated) */
typedef struct {
    char *data;
    uint32_t length;
    uint32_t hash;
} VMString;

/* Runtime value - must be defined before types that embed it */
typedef struct Value {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        VMString string;
        VMArray *array;
        VMObject *object;
        VMIterator *iterator;
    } as;
} Value;

/* Array value */
struct VMArray {
    Value *elements;
    uint32_t count;
    uint32_t capacity;
};

/* Object field */
typedef struct {
    char *key;
    uint32_t key_hash;
    Value value;
} ObjectField;

/* Object value */
struct VMObject {
    ObjectField *fields;
    uint32_t count;
    uint32_t capacity;
};

/* Iterator for for-loops */
struct VMIterator {
    Value *array;         /* Source array */
    uint32_t count;
    uint32_t current;     /* Current index */
};

/* Call frame for function calls */
typedef enum {
    VM_FRAME_MAIN = 0,
    VM_FRAME_FUNCTION = 1,
    VM_FRAME_COMPONENT = 2,
} VMFrameKind;

typedef struct {
    Chunk *chunk;         /* Current chunk being executed */
    uint8_t *ip;          /* Instruction pointer */
    Value *slots;         /* Base of local variables on stack */
    VMFrameKind kind;     /* Main/function/component frame */
    uint16_t func_idx;    /* Function index for function/component frames */
} CallFrame;

typedef struct {
    char *data;
    uint32_t len;
    uint32_t cap;
} VMCaptureBuffer;

/* Output callback for streaming HTML */
typedef void (*VMOutputFn)(const char *data, uint32_t len, void *userdata);

/* Data fetch callback for SQL queries */
typedef Value (*VMFetchDataFn)(const DataRequirement *req, const Value *frame_slots, void *userdata);
typedef Value (*VMComponentLoadFn)(const ComponentRef *ref, const Value *args,
                                   uint8_t argc, void *userdata);

typedef Value (*VMComponentLinkedLoadFn)(const ComponentRef *ref, const Value *args,
                                         uint8_t argc, void *userdata);

/* Static component boundary callbacks (for debug attribution/tracing) */
typedef void (*VMComponentStartFn)(uint16_t func_idx, void *userdata);
typedef void (*VMComponentEndFn)(uint16_t func_idx, void *userdata);
typedef void (*VMSlotDefaultStartFn)(uint16_t func_idx, void *userdata);
typedef void (*VMSlotDefaultEndFn)(uint16_t func_idx, void *userdata);
typedef void (*VMStepHookFn)(uint8_t chunk_kind,
                             uint16_t chunk_index,
                             uint32_t pc,
                             uint8_t opcode,
                             uint32_t line,
                             uint32_t column,
                             const char *source_path,
                             void *userdata);

/* VM state */
typedef struct {
    Arena *arena;

    /* Bytecode module */
    BytecodeModule *module;

    /* Value stack */
    Value stack[VM_STACK_MAX];
    Value *stack_top;

    /* Call frames */
    CallFrame frames[VM_FRAMES_MAX];
    int frame_count;

    /* Slot capture stack (for default slot content) */
    VMCaptureBuffer captures[VM_CAPTURE_MAX];
    int capture_depth;

    /* Output */
    VMOutputFn output_fn;
    void *output_userdata;

    /* Data fetching */
    VMFetchDataFn fetch_fn;
    void *fetch_userdata;

    /* Dynamic component loading */
    VMComponentLoadFn component_fn;
    void *component_userdata;

    /* Linked component loading */
    VMComponentLinkedLoadFn linked_component_fn;
    void *linked_component_userdata;
    uint16_t *linked_component_func_targets; /* ref_idx -> func_idx, LINKED_NONE if unresolved */

    /* Static component boundaries */
    VMComponentStartFn component_start_fn;
    VMComponentEndFn component_end_fn;
    VMSlotDefaultStartFn slot_default_start_fn;
    VMSlotDefaultEndFn slot_default_end_fn;
    void *component_boundary_userdata;
    VMStepHookFn step_hook_fn;
    void *step_hook_userdata;

    /* Async await state (used by wasm host adapters) */
    bool awaiting;

    /* Globals (for data fetching) */
    VMObject *globals;

    /* Error handling */
    bool had_error;
    char error_msg[256];

    /* Statistics */
    uint64_t instructions_executed;
} VM;

/* VM lifecycle */
VM *vm_new(Arena *arena);
void vm_init(VM *vm, BytecodeModule *module);
void vm_set_output(VM *vm, VMOutputFn fn, void *userdata);
void vm_set_fetch(VM *vm, VMFetchDataFn fn, void *userdata);
void vm_set_component_loader(VM *vm, VMComponentLoadFn fn, void *userdata);
void vm_set_linked_component_loader(VM *vm, VMComponentLinkedLoadFn fn, void *userdata);
void vm_set_component_boundary_hooks(
    VM *vm,
    VMComponentStartFn start_fn,
    VMComponentEndFn end_fn,
    void *userdata
);
void vm_set_slot_default_hooks(
    VM *vm,
    VMSlotDefaultStartFn start_fn,
    VMSlotDefaultEndFn end_fn,
    void *userdata
);
void vm_set_step_hook(VM *vm, VMStepHookFn fn, void *userdata);

/* Async helpers for host adapters */
void vm_request_await(VM *vm);
void vm_clear_await(VM *vm);
bool vm_is_awaiting(VM *vm);
void vm_replace_top(VM *vm, Value value);
void vm_emit_raw(VM *vm, const char *data, uint32_t len);

/* Execution */
typedef enum {
    VM_OK,
    VM_ERROR,
    VM_AWAIT_DATA,    /* Waiting for async data */
} VMResult;

VMResult vm_run(VM *vm);
VMResult vm_step(VM *vm);    /* Execute single instruction */

/* Value constructors */
Value val_null(void);
Value val_bool(bool value);
Value val_int(int64_t value);
Value val_number(double value);
Value val_string(VM *vm, const char *data, uint32_t length);
Value val_array(VM *vm, uint32_t capacity);
Value val_object(VM *vm, uint32_t capacity);

/* Value operations */
bool val_is_truthy(Value *value);
bool val_equals(Value *a, Value *b);
int val_compare(Value *a, Value *b);  /* <0, 0, >0 */
void val_print(Value *value);         /* Debug output */
Value val_to_string(VM *vm, Value *value);

/* Array operations */
void array_push(VM *vm, VMArray *arr, Value value);
Value array_get(VMArray *arr, uint32_t index);
void array_set(VMArray *arr, uint32_t index, Value value);

/* Object operations */
void object_set(VM *vm, VMObject *obj, const char *key, Value value);
Value *object_get(VMObject *obj, const char *key);
bool object_has(VMObject *obj, const char *key);

/* Global data (for SQL results, etc.) */
void vm_set_global(VM *vm, const char *name, Value value);
Value *vm_get_global(VM *vm, const char *name);

/* Error reporting */
void vm_error(VM *vm, const char *fmt, ...);
const char *vm_error_message(VM *vm);

#endif /* MOT_VM_H */
