/*
 * Bytecode format for Motus
 *
 * Streamable bytecode designed for WASM edge execution.
 * Supports partial evaluation with "holes" for dynamic data.
 */

#ifndef MOT_BYTECODE_H
#define MOT_BYTECODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../util/arena.h"

/* Magic number: "MOT\0" */
#define BYTECODE_MAGIC 0x00544F4D

/* Version */
#define BYTECODE_VERSION_MAJOR 1
#define BYTECODE_VERSION_MINOR 3

/* Optional debug trailer magic: "MDBG" */
#define BYTECODE_DEBUG_MAGIC 0x4742444Du

/* Bytecode flags */
#define BYTECODE_FLAG_AUTH_REQUIRED  0x0001  /* Page requires authentication */
#define BYTECODE_FLAG_ADMIN_REQUIRED 0x0002  /* Page requires admin role */

/* Opcodes */
typedef enum {
    /* Stack operations */
    BC_NOP = 0,          /* No operation */
    BC_CONST,            /* Push constant: idx(u16) */
    BC_POP,              /* Pop top of stack */
    BC_DUP,              /* Duplicate top of stack */

    /* Variables */
    BC_LOAD,             /* Load local: slot(u16) */
    BC_STORE,            /* Store local: slot(u16) */
    BC_LOAD_GLOBAL,      /* Load global: idx(u16) */
    BC_LOAD_FIELD,       /* obj.field: field_idx(u16) */
    BC_LOAD_INDEX,       /* arr[i]: (stack: arr, idx) */
    BC_STORE_FIELD,      /* obj.field = val: field_idx(u16), (stack: val, obj) */
    BC_STORE_INDEX,      /* arr[i] = val: (stack: val, arr, idx) */

    /* Literals */
    BC_NULL,             /* Push null */
    BC_TRUE,             /* Push true */
    BC_FALSE,            /* Push false */
    BC_INT,              /* Push small int: val(i16) */

    /* Arithmetic */
    BC_ADD,              /* a + b */
    BC_SUB,              /* a - b */
    BC_MUL,              /* a * b */
    BC_DIV,              /* a / b */
    BC_MOD,              /* a % b */
    BC_NEG,              /* -a */

    /* Comparison */
    BC_EQ,               /* a == b */
    BC_NEQ,              /* a != b */
    BC_LT,               /* a < b */
    BC_LTE,              /* a <= b */
    BC_GT,               /* a > b */
    BC_GTE,              /* a >= b */

    /* Logic */
    BC_AND,              /* a and b (short-circuit) */
    BC_OR,               /* a or b (short-circuit) */
    BC_NOT,              /* not a */

    /* Control flow */
    BC_JUMP,             /* Unconditional: offset(i16) */
    BC_JUMP_IF_FALSE,    /* Conditional: offset(i16) */
    BC_JUMP_IF_TRUE,     /* Conditional: offset(i16) */

    /* Iteration */
    BC_ITER_START,       /* Begin iteration (stack: iterable) */
    BC_ITER_NEXT,        /* Next item or jump: offset(i16) */
    BC_ITER_END,         /* End iteration */

    /* Output (streaming) */
    BC_EMIT_LITERAL,     /* Emit raw HTML: const_idx(u16) */
    BC_EMIT_TEXT,        /* Emit escaped text (stack: value) */
    BC_EMIT_RAW,         /* Emit unescaped HTML (stack: value) */
    BC_EMIT_ATTR_START,  /* Begin attribute: name_idx(u16) */
    BC_EMIT_ATTR_END,    /* End attribute (emit ") */
    BC_EMIT_TAG_OPEN,    /* Emit <tag (no closing >): name_idx(u16) */
    BC_EMIT_TAG_END,     /* Emit > to close opening tag */
    BC_EMIT_TAG_CLOSE,   /* Emit </tag>: name_idx(u16) */
    BC_EMIT_TAG_SELF,    /* Emit /> to close self-closing tag */

    /* Data fetching (runtime holes) */
    BC_FETCH_DATA,       /* Request data: data_req_idx(u16) */
    BC_FETCH_WAIT,       /* Wait for data to be available */

    /* Functions */
    BC_CALL,             /* Call function: fn_idx(u16), argc(u8) */
    BC_CALL_BUILTIN,     /* Call builtin: builtin_idx(u16), argc(u8) */
    BC_CALL_PIPE,        /* Pipe call: fn_idx(u16) (implicit 1 arg) */
    BC_RETURN,           /* Return from function */

    /* Components */
    BC_COMPONENT_START,  /* Start component: comp_idx(u16) */
    BC_COMPONENT_END,    /* End component */
    BC_COMPONENT_LOAD,   /* Load dynamic component: ref_idx(u16), argc(u8) */
    BC_SLOT_START,       /* Start slot: slot_idx(u16) */
    BC_SLOT_END,         /* End slot */
    BC_SLOT_DEFAULT,     /* Use default slot content */

    /* Dependency tracking */
    BC_DEP_START,        /* Begin dependency region: dep_idx(u16) */
    BC_DEP_END,          /* End dependency region */

    /* Arrays/Objects */
    BC_ARRAY_NEW,        /* Create array: count(u16) */
    BC_OBJECT_NEW,       /* Create object: count(u16) */
    BC_OBJECT_SET,       /* Set object field: key_idx(u16) */

    /* String operations */
    BC_CONCAT,           /* Concatenate strings: count(u8) */

    BC_HALT,             /* End of program */
    BC_COMPONENT_LINKED, /* Load linked component: ref_idx(u16), argc(u8) */

    /* Mutation forms */
    BC_MUTATE_START,     /* Begin mutation form: mut_req_idx(u16) */
    BC_MUTATE_END,       /* End mutation form */
} OpCode;

/* Constant types */
typedef enum {
    CONST_NULL,
    CONST_BOOL,
    CONST_INT,
    CONST_NUMBER,
    CONST_STRING,
} ConstType;

/* Constant value */
typedef struct {
    ConstType type;
    union {
        bool boolean;
        int64_t integer;
        double number;
        struct {
            char *data;
            uint32_t length;
        } string;
    } v;
} Constant;

/* Data requirement (runtime SQL query) */
typedef struct {
    char *name;              /* Binding name */
    uint32_t query_ref;      /* Stable query reference id for origin RPC */
    uint32_t signature;      /* Query+param signature for RPC validation */
    char *query;             /* SQL query string (compile-time/origin-side only) */
    uint32_t query_len;      /* 0 after bytecode deserialization in edge runtime */
    uint32_t line;           /* Source line for debug metadata */
    uint32_t column;         /* Source column for debug metadata */
    char **param_names;      /* Parameter names (without leading ':') */
    uint16_t *param_slots;   /* Local slot for each parameter */
    uint16_t param_count;
    bool is_single;          /* Single row vs array */
    bool is_dynamic;         /* Dynamic (runtime) data */
} DataRequirement;

/* Dependency path */
typedef struct {
    char *path;              /* e.g., "product.name" */
    uint32_t path_len;
} Dependency;

/* Builtin function reference */
typedef struct {
    char *name;
    uint8_t min_args;
    uint8_t max_args;
} BuiltinRef;

/* Mutation types */
typedef enum {
    MUTATE_INSERT,
    MUTATE_UPDATE,
    MUTATE_DELETE,
} MutationType;

/* Mutation requirement */
typedef struct {
    MutationType type;
    char *target;                /* Binding name (e.g., "contacts") */
    char **field_names;          /* Bound input field names */
    uint16_t field_count;
    uint16_t field_cap;
} MutationRequirement;

/* Dynamic component reference (loaded at edge) */
typedef struct {
    char *name;              /* Component name (e.g., "Header") */
    char *path;              /* Path to load from (e.g., "components/Header") */
    uint32_t name_len;
    uint32_t path_len;
} ComponentRef;

/* Static function/component debug metadata */
typedef struct {
    char *name;              /* Component/function display name */
    char *source_path;       /* Source file path for this function chunk */
    uint32_t name_len;
    uint32_t source_path_len;
} FunctionDebugRef;

/* Debug span (maps chunk pc range to source location) */
typedef struct {
    uint8_t chunk_kind;      /* 0: main, 1: function */
    uint16_t chunk_index;    /* function index when chunk_kind=1 */
    uint32_t start_pc;       /* inclusive */
    uint32_t end_pc;         /* exclusive */
    uint32_t line;           /* 1-based source line */
    uint32_t column;         /* 1-based source column */
    uint16_t node_type;      /* reserved */
} BytecodeDebugSpan;

/* Bytecode chunk (section of code) */
typedef struct {
    uint8_t *code;           /* Bytecode instructions */
    uint32_t code_len;
    uint32_t code_cap;

    /* Line info for debugging */
    uint32_t *lines;         /* Line number for each instruction */
    uint32_t lines_len;
    uint32_t lines_cap;
} Chunk;

/* Bytecode module */
typedef struct {
    /* Header */
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t flags;

    /* Constants pool */
    Constant *constants;
    uint32_t const_count;
    uint32_t const_cap;

    /* String table (interned strings) */
    char **strings;
    uint32_t string_count;
    uint32_t string_cap;

    /* Data requirements (SQL queries) */
    DataRequirement *data_reqs;
    uint32_t data_req_count;
    uint32_t data_req_cap;

    /* Dependencies for cache invalidation */
    Dependency *deps;
    uint32_t dep_count;
    uint32_t dep_cap;

    /* Builtin function references */
    BuiltinRef *builtins;
    uint32_t builtin_count;
    uint32_t builtin_cap;

    /* Dynamic component references */
    ComponentRef *comp_refs;
    uint32_t comp_ref_count;
    uint32_t comp_ref_cap;

    /* Mutation requirements */
    MutationRequirement *mut_reqs;
    uint32_t mut_req_count;
    uint32_t mut_req_cap;

    /* Main code chunk */
    Chunk main;

    /* Function chunks (for components, macros) */
    Chunk *functions;
    uint32_t func_count;
    uint32_t func_cap;
    FunctionDebugRef *func_debug;
    uint32_t func_debug_cap;
    BytecodeDebugSpan *debug_spans;
    uint32_t debug_span_count;

    /* Arena for allocations */
    Arena *arena;
} BytecodeModule;

/* Create a new bytecode module */
BytecodeModule *bytecode_module_new(Arena *arena);

/* Chunk operations */
void chunk_init(Chunk *chunk, Arena *arena);
void chunk_write(Chunk *chunk, uint8_t byte, uint32_t line, Arena *arena);
void chunk_write_u16(Chunk *chunk, uint16_t value, uint32_t line, Arena *arena);
void chunk_write_i16(Chunk *chunk, int16_t value, uint32_t line, Arena *arena);
uint32_t chunk_current_offset(Chunk *chunk);
void chunk_patch_jump(Chunk *chunk, uint32_t offset, int16_t jump);

/* Constants */
uint16_t bytecode_add_constant(BytecodeModule *mod, Constant constant);
uint16_t bytecode_add_string(BytecodeModule *mod, const char *str, uint32_t len);
uint16_t bytecode_add_number(BytecodeModule *mod, double value);
uint16_t bytecode_add_int(BytecodeModule *mod, int64_t value);

/* Data requirements */
uint16_t bytecode_add_data_req(BytecodeModule *mod, const char *name,
                               const char *query, uint32_t query_len,
                               bool is_single, bool is_dynamic,
                               uint32_t line, uint32_t column,
                               uint16_t param_count,
                               const char **param_names,
                               const uint16_t *param_slots);

/* Dependencies */
uint16_t bytecode_add_dependency(BytecodeModule *mod, const char *path);

/* Builtins */
uint16_t bytecode_add_builtin(BytecodeModule *mod, const char *name,
                              uint8_t min_args, uint8_t max_args);
int bytecode_find_builtin(BytecodeModule *mod, const char *name);

/* Mutation requirements */
uint16_t bytecode_add_mutation_req(BytecodeModule *mod, MutationType type, const char *target);
void bytecode_mutation_req_add_field(BytecodeModule *mod, uint16_t idx, const char *field_name);

/* Dynamic component references */
uint16_t bytecode_add_comp_ref(BytecodeModule *mod, const char *name, const char *path);

/* Functions */
uint16_t bytecode_add_function(BytecodeModule *mod);
Chunk *bytecode_get_function(BytecodeModule *mod, uint16_t idx);
void bytecode_set_function_debug(BytecodeModule *mod, uint16_t idx,
                                 const char *name, const char *source_path);

/* Serialization */
uint8_t *bytecode_serialize(BytecodeModule *mod, uint32_t *out_len);
uint8_t *bytecode_serialize_ex(BytecodeModule *mod, uint32_t *out_len, bool include_debug);
BytecodeModule *bytecode_deserialize(const uint8_t *data, uint32_t len, Arena *arena);

/* Debug lookup */
bool bytecode_find_debug_span(const BytecodeModule *mod,
                              uint8_t chunk_kind,
                              uint16_t chunk_index,
                              uint32_t pc,
                              BytecodeDebugSpan *out_span);

/* Debug */
void bytecode_disassemble(BytecodeModule *mod);
void chunk_disassemble(Chunk *chunk, const char *name, BytecodeModule *mod);
const char *opcode_name(OpCode op);

#endif /* MOT_BYTECODE_H */
