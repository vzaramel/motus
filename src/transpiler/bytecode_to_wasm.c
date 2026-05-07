/*
 * Bytecode -> WASM transpiler (C core).
 *
 * Emits an executable wasm module with:
 * - lowered opcode dispatch in wasm (no generic step callback)
 * - export: mot_component_run() -> i32
 * - export: memory
 * - custom section: mot.native_meta (constants/strings/data-req/deps/comp-refs/func ranges)
 */

#include "bytecode_to_wasm.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "../compiler/bytecode.h"
#include "../util/arena.h"

#define NATIVE_META_MAGIC 0x4d544e59u /* "YNTM" */
#define NATIVE_META_VERSION 1u

#define MOT_OK 0
#define MOT_ERROR -1
#define MOT_AWAIT -2

#define WASM_SECTION_TYPE 1u
#define WASM_SECTION_IMPORT 2u
#define WASM_SECTION_FUNCTION 3u
#define WASM_SECTION_MEMORY 5u
#define WASM_SECTION_GLOBAL 6u
#define WASM_SECTION_EXPORT 7u
#define WASM_SECTION_CODE 10u

#define WASM_VALTYPE_I32 0x7f

#define WASM_OP_BLOCK 0x02
#define WASM_OP_LOOP 0x03
#define WASM_OP_IF 0x04
#define WASM_OP_ELSE 0x05
#define WASM_OP_END 0x0b
#define WASM_OP_BR 0x0c
#define WASM_OP_BR_IF 0x0d
#define WASM_OP_RETURN 0x0f
#define WASM_OP_CALL 0x10
#define WASM_OP_DROP 0x1a
#define WASM_OP_LOCAL_GET 0x20
#define WASM_OP_LOCAL_SET 0x21
#define WASM_OP_GLOBAL_GET 0x23
#define WASM_OP_GLOBAL_SET 0x24
#define WASM_OP_I32_LOAD 0x28
#define WASM_OP_I32_STORE 0x36
#define WASM_OP_I32_CONST 0x41
#define WASM_OP_I32_EQZ 0x45
#define WASM_OP_I32_EQ 0x46
#define WASM_OP_I32_LT_S 0x48
#define WASM_OP_I32_GE_U 0x4f
#define WASM_OP_I32_ADD 0x6a
#define WASM_OP_I32_SUB 0x6b
#define WASM_OP_I32_MUL 0x6c

#define STACK_BASE_BYTES 0u
#define STACK_CAP 8192u
#define CALL_BASE_BYTES (STACK_CAP * 4u)
#define CALL_CAP 256u
#define CALL_ENTRY_BYTES 16u

typedef enum {
    IMP_INPUT_PROPS = 0,
    IMP_INPUT_CHILDREN,
    IMP_CONST,
    IMP_NULL,
    IMP_TRUE,
    IMP_FALSE,
    IMP_INT,
    IMP_LOAD_GLOBAL,
    IMP_IS_TRUTHY,
    IMP_UNARY,
    IMP_BINARY,
    IMP_LOAD_FIELD,
    IMP_LOAD_INDEX,
    IMP_STORE_FIELD,
    IMP_STORE_INDEX,
    IMP_EMIT_LITERAL,
    IMP_EMIT_TEXT,
    IMP_EMIT_RAW,
    IMP_EMIT_ATTR_START,
    IMP_EMIT_ATTR_END,
    IMP_EMIT_TAG_OPEN,
    IMP_EMIT_TAG_END,
    IMP_EMIT_TAG_CLOSE,
    IMP_EMIT_TAG_SELF,
    IMP_ITER_START,
    IMP_ITER_HAS_NEXT,
    IMP_ITER_NEXT_VALUE,
    IMP_FETCH_DATA,
    IMP_FETCH_DATA_RESULT,
    IMP_CALL_BUILTIN,
    IMP_COMPONENT_LOAD,
    IMP_COMPONENT_LINKED,
    IMP_DEP_START,
    IMP_DEP_END,
    IMP_ARRAY_NEW,
    IMP_CONCAT,
    IMP_OBJ_NEW,
    IMP_OBJ_SET,
    IMP_SLOT_START,
    IMP_SLOT_END,
    IMP_SLOT_DEFAULT,
    IMP_COUNT
} ImportIndex;

enum {
    NATIVE_UNARY_NEG = 1,
    NATIVE_UNARY_NOT = 2
};

enum {
    NATIVE_BINARY_ADD = 1,
    NATIVE_BINARY_SUB,
    NATIVE_BINARY_MUL,
    NATIVE_BINARY_DIV,
    NATIVE_BINARY_MOD,
    NATIVE_BINARY_EQ,
    NATIVE_BINARY_NEQ,
    NATIVE_BINARY_LT,
    NATIVE_BINARY_LTE,
    NATIVE_BINARY_GT,
    NATIVE_BINARY_GTE,
    NATIVE_BINARY_AND,
    NATIVE_BINARY_OR
};

typedef enum {
    G_PC = 0,
    G_SP,
    G_CALL_SP,
    G_FRAME_BASE,
    G_CODE_END,
    G_INITIALIZED,
    G_COUNT
} GlobalIndex;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

typedef struct {
    uint8_t opcode;
    int32_t a;
    int32_t b;
} NativeInstruction;

typedef struct {
    NativeInstruction *items;
    uint32_t count;
    uint32_t cap;
} InstructionVec;

typedef struct {
    uint32_t start;
    uint32_t end;
} FunctionRange;

static void set_error(char *buf, size_t cap, const char *msg) {
    if (!buf || cap == 0) return;
    if (!msg) {
        buf[0] = '\0';
        return;
    }
    (void)snprintf(buf, cap, "%s", msg);
}

static bool buf_reserve(Buf *buf, size_t need) {
    size_t next_cap;
    uint8_t *next_data;
    if (!buf) return false;
    if (buf->len + need <= buf->cap) return true;
    next_cap = buf->cap ? buf->cap : 256u;
    while (next_cap < buf->len + need) {
        if (next_cap > ((size_t)-1) / 2u) return false;
        next_cap *= 2u;
    }
    next_data = (uint8_t *)realloc(buf->data, next_cap);
    if (!next_data) return false;
    buf->data = next_data;
    buf->cap = next_cap;
    return true;
}

static bool buf_put_u8(Buf *buf, uint8_t v) {
    if (!buf_reserve(buf, 1u)) return false;
    buf->data[buf->len++] = v;
    return true;
}

static bool buf_put_bytes(Buf *buf, const void *data, size_t len) {
    if (len == 0) return true;
    if (!buf_reserve(buf, len)) return false;
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return true;
}

static bool buf_put_u16_le(Buf *buf, uint16_t v) {
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(v & 0xffu);
    bytes[1] = (uint8_t)((v >> 8) & 0xffu);
    return buf_put_bytes(buf, bytes, sizeof(bytes));
}

static bool buf_put_u32_le(Buf *buf, uint32_t v) {
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(v & 0xffu);
    bytes[1] = (uint8_t)((v >> 8) & 0xffu);
    bytes[2] = (uint8_t)((v >> 16) & 0xffu);
    bytes[3] = (uint8_t)((v >> 24) & 0xffu);
    return buf_put_bytes(buf, bytes, sizeof(bytes));
}

static bool buf_put_u64_le(Buf *buf, uint64_t v) {
    uint8_t bytes[8];
    bytes[0] = (uint8_t)(v & 0xffu);
    bytes[1] = (uint8_t)((v >> 8) & 0xffu);
    bytes[2] = (uint8_t)((v >> 16) & 0xffu);
    bytes[3] = (uint8_t)((v >> 24) & 0xffu);
    bytes[4] = (uint8_t)((v >> 32) & 0xffu);
    bytes[5] = (uint8_t)((v >> 40) & 0xffu);
    bytes[6] = (uint8_t)((v >> 48) & 0xffu);
    bytes[7] = (uint8_t)((v >> 56) & 0xffu);
    return buf_put_bytes(buf, bytes, sizeof(bytes));
}

static bool buf_put_f64_le(Buf *buf, double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    return buf_put_u64_le(buf, bits);
}

static bool buf_put_uleb32(Buf *buf, uint32_t value) {
    uint8_t out[5];
    size_t n = 0;
    uint32_t v = value;
    do {
        uint8_t b = (uint8_t)(v & 0x7fu);
        v >>= 7;
        if (v != 0) b |= 0x80u;
        out[n++] = b;
    } while (v != 0 && n < sizeof(out));
    return buf_put_bytes(buf, out, n);
}

static bool buf_put_sleb32(Buf *buf, int32_t value) {
    uint8_t out[5];
    size_t n = 0;
    int32_t v = value;
    bool more = true;
    while (more && n < sizeof(out)) {
        uint8_t byte = (uint8_t)(v & 0x7f);
        int32_t sign = v >> 31;
        v >>= 7;
        if ((v == sign) && ((byte & 0x40u) == (uint8_t)(sign & 0x40u))) {
            more = false;
        } else {
            byte |= 0x80u;
        }
        out[n++] = byte;
    }
    return buf_put_bytes(buf, out, n);
}

static bool buf_put_name(Buf *buf, const char *name) {
    size_t len = name ? strlen(name) : 0;
    if (len > 0xffffffffu) return false;
    if (!buf_put_uleb32(buf, (uint32_t)len)) return false;
    return buf_put_bytes(buf, name, len);
}

static uint16_t read_u16_le_bytes(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0]) | (((uint16_t)p[1]) << 8));
}

static int16_t read_i16_le_bytes(const uint8_t *p) {
    return (int16_t)read_u16_le_bytes(p);
}

static int opcode_operand_size(uint8_t op) {
    switch (op) {
        case BC_CONST:
        case BC_LOAD:
        case BC_STORE:
        case BC_LOAD_GLOBAL:
        case BC_LOAD_FIELD:
        case BC_STORE_FIELD:
        case BC_INT:
        case BC_JUMP:
        case BC_JUMP_IF_FALSE:
        case BC_JUMP_IF_TRUE:
        case BC_ITER_NEXT:
        case BC_EMIT_LITERAL:
        case BC_EMIT_ATTR_START:
        case BC_EMIT_TAG_OPEN:
        case BC_EMIT_TAG_CLOSE:
        case BC_FETCH_DATA:
        case BC_COMPONENT_START:
        case BC_SLOT_START:
        case BC_DEP_START:
        case BC_ARRAY_NEW:
        case BC_OBJECT_NEW:
        case BC_OBJECT_SET:
            return 2;
        case BC_CALL:
        case BC_CALL_BUILTIN:
        case BC_COMPONENT_LOAD:
        case BC_COMPONENT_LINKED:
            return 3;
        case BC_CALL_PIPE:
            return 2;
        case BC_CONCAT:
            return 1;
        default:
            return 0;
    }
}

static bool instruction_vec_push(InstructionVec *vec, NativeInstruction inst) {
    NativeInstruction *next;
    uint32_t next_cap;
    if (!vec) return false;
    if (vec->count >= vec->cap) {
        next_cap = vec->cap ? (vec->cap * 2u) : 256u;
        next = (NativeInstruction *)realloc(vec->items, sizeof(NativeInstruction) * (size_t)next_cap);
        if (!next) return false;
        vec->items = next;
        vec->cap = next_cap;
    }
    vec->items[vec->count++] = inst;
    return true;
}

static bool decode_chunk_instructions(const uint8_t *code,
                                      uint32_t code_len,
                                      uint32_t base_index,
                                      InstructionVec *out,
                                      uint32_t *out_count,
                                      char *error_buf,
                                      size_t error_buf_len) {
    int32_t *offset_to_instr = NULL;
    uint32_t off = 0;
    uint32_t local_count = 0;
    uint32_t i;
    if (!code && code_len > 0u) {
        set_error(error_buf, error_buf_len, "invalid chunk code pointer");
        return false;
    }
    offset_to_instr = (int32_t *)malloc(sizeof(int32_t) * (size_t)(code_len + 1u));
    if (!offset_to_instr) {
        set_error(error_buf, error_buf_len, "out of memory building instruction map");
        return false;
    }
    for (i = 0; i <= code_len; i++) offset_to_instr[i] = -1;

    while (off < code_len) {
        uint8_t op = code[off];
        int operand_size = opcode_operand_size(op);
        uint32_t insn_size = 1u + (uint32_t)((operand_size < 0) ? 0 : operand_size);
        if (off + insn_size > code_len) {
            free(offset_to_instr);
            set_error(error_buf, error_buf_len, "truncated opcode stream while indexing instructions");
            return false;
        }
        offset_to_instr[off] = (int32_t)local_count;
        local_count++;
        off += insn_size;
    }
    if (off != code_len) {
        free(offset_to_instr);
        set_error(error_buf, error_buf_len, "invalid opcode stream length");
        return false;
    }
    offset_to_instr[code_len] = (int32_t)local_count;

    off = 0;
    while (off < code_len) {
        uint8_t op = code[off];
        uint32_t next_off;
        NativeInstruction inst;
        inst.opcode = op;
        inst.a = 0;
        inst.b = 0;
        next_off = off + 1u + (uint32_t)opcode_operand_size(op);
        switch (op) {
            case BC_CONST:
            case BC_LOAD:
            case BC_STORE:
            case BC_LOAD_GLOBAL:
            case BC_LOAD_FIELD:
            case BC_STORE_FIELD:
            case BC_EMIT_LITERAL:
            case BC_EMIT_ATTR_START:
            case BC_EMIT_TAG_OPEN:
            case BC_EMIT_TAG_CLOSE:
            case BC_FETCH_DATA:
            case BC_COMPONENT_START:
            case BC_SLOT_START:
            case BC_DEP_START:
            case BC_ARRAY_NEW:
            case BC_OBJECT_NEW:
            case BC_OBJECT_SET:
                inst.a = (int32_t)read_u16_le_bytes(code + off + 1u);
                break;
            case BC_INT:
                inst.a = (int32_t)read_i16_le_bytes(code + off + 1u);
                break;
            case BC_CALL:
            case BC_CALL_BUILTIN:
            case BC_COMPONENT_LOAD:
            case BC_COMPONENT_LINKED:
                inst.a = (int32_t)read_u16_le_bytes(code + off + 1u);
                inst.b = (int32_t)code[off + 3u];
                break;
            case BC_CALL_PIPE:
                inst.a = (int32_t)read_u16_le_bytes(code + off + 1u);
                break;
            case BC_CONCAT:
                inst.a = (int32_t)code[off + 1u];
                break;
            case BC_JUMP:
            case BC_JUMP_IF_FALSE:
            case BC_JUMP_IF_TRUE:
            case BC_ITER_NEXT: {
                int16_t rel = read_i16_le_bytes(code + off + 1u);
                int64_t target_off = (int64_t)next_off + (int64_t)rel;
                int32_t local_target;
                if (target_off < 0 || target_off > (int64_t)code_len) {
                    free(offset_to_instr);
                    set_error(error_buf, error_buf_len, "jump target out of chunk bounds");
                    return false;
                }
                local_target = offset_to_instr[(uint32_t)target_off];
                if (local_target < 0) {
                    free(offset_to_instr);
                    set_error(error_buf, error_buf_len, "jump target does not align to instruction boundary");
                    return false;
                }
                inst.a = (int32_t)(base_index + (uint32_t)local_target);
                break;
            }
            default:
                break;
        }

        if (!instruction_vec_push(out, inst)) {
            free(offset_to_instr);
            set_error(error_buf, error_buf_len, "out of memory storing decoded instructions");
            return false;
        }
        off = next_off;
    }

    free(offset_to_instr);
    if (out_count) *out_count = local_count;
    return true;
}

static bool write_meta_string(Buf *meta, const char *s, uint32_t explicit_len) {
    uint32_t len = explicit_len;
    if (!s) {
        len = 0;
    } else if (len == 0xffffffffu) {
        size_t n = strlen(s);
        if (n > 0xffffffffu) return false;
        len = (uint32_t)n;
    }
    if (!buf_put_u32_le(meta, len)) return false;
    if (len == 0) return true;
    return buf_put_bytes(meta, s, len);
}

static bool emit_native_meta_section(const BytecodeModule *mod,
                                     const FunctionRange *functions,
                                     uint32_t function_count,
                                     uint32_t main_end_index,
                                     uint32_t instruction_count,
                                     Buf *out_meta,
                                     char *error_buf,
                                     size_t error_buf_len) {
    uint32_t i, p;
    if (!buf_put_u32_le(out_meta, NATIVE_META_MAGIC) ||
        !buf_put_u16_le(out_meta, NATIVE_META_VERSION)) {
        set_error(error_buf, error_buf_len, "failed to write native meta header");
        return false;
    }

    if (!buf_put_u32_le(out_meta, mod->const_count)) return false;
    for (i = 0; i < mod->const_count; i++) {
        const Constant *c = &mod->constants[i];
        if (!buf_put_u8(out_meta, (uint8_t)c->type)) return false;
        switch (c->type) {
            case CONST_NULL:
                break;
            case CONST_BOOL:
                if (!buf_put_u8(out_meta, c->v.boolean ? 1u : 0u)) return false;
                break;
            case CONST_INT:
                if (!buf_put_u64_le(out_meta, (uint64_t)c->v.integer)) return false;
                break;
            case CONST_NUMBER:
                if (!buf_put_f64_le(out_meta, c->v.number)) return false;
                break;
            case CONST_STRING:
                if (!write_meta_string(out_meta, c->v.string.data, c->v.string.length)) return false;
                break;
            default:
                set_error(error_buf, error_buf_len, "unsupported constant type in native meta");
                return false;
        }
    }

    if (!buf_put_u32_le(out_meta, mod->string_count)) return false;
    for (i = 0; i < mod->string_count; i++) {
        if (!write_meta_string(out_meta, mod->strings[i], 0xffffffffu)) return false;
    }

    if (!buf_put_u32_le(out_meta, mod->data_req_count)) return false;
    for (i = 0; i < mod->data_req_count; i++) {
        const DataRequirement *req = &mod->data_reqs[i];
        if (!write_meta_string(out_meta, req->name, 0xffffffffu)) return false;
        if (!buf_put_u32_le(out_meta, req->query_ref)) return false;
        if (!buf_put_u32_le(out_meta, req->signature)) return false;
        if (!buf_put_u8(out_meta, req->is_single ? 1u : 0u)) return false;
        if (!buf_put_u16_le(out_meta, req->param_count)) return false;
        for (p = 0; p < req->param_count; p++) {
            const char *param_name = req->param_names ? req->param_names[p] : "";
            uint16_t param_slot = req->param_slots ? req->param_slots[p] : 0u;
            if (!write_meta_string(out_meta, param_name, 0xffffffffu)) return false;
            if (!buf_put_u16_le(out_meta, param_slot)) return false;
        }
    }

    if (!buf_put_u32_le(out_meta, mod->dep_count)) return false;
    for (i = 0; i < mod->dep_count; i++) {
        if (!write_meta_string(out_meta, mod->deps[i].path, mod->deps[i].path_len)) return false;
    }

    if (!buf_put_u32_le(out_meta, mod->comp_ref_count)) return false;
    for (i = 0; i < mod->comp_ref_count; i++) {
        if (!write_meta_string(out_meta, mod->comp_refs[i].name, mod->comp_refs[i].name_len)) return false;
        if (!write_meta_string(out_meta, mod->comp_refs[i].path, mod->comp_refs[i].path_len)) return false;
    }

    if (!buf_put_u32_le(out_meta, function_count)) return false;
    for (i = 0; i < function_count; i++) {
        if (!buf_put_u32_le(out_meta, functions[i].start)) return false;
        if (!buf_put_u32_le(out_meta, functions[i].end)) return false;
    }

    if (!buf_put_u32_le(out_meta, main_end_index)) return false;
    if (!buf_put_u32_le(out_meta, instruction_count)) return false;
    return true;
}

static bool append_wasm_section(Buf *wasm, uint8_t section_id, const Buf *payload) {
    if (!buf_put_u8(wasm, section_id)) return false;
    if (!buf_put_uleb32(wasm, (uint32_t)payload->len)) return false;
    return buf_put_bytes(wasm, payload->data, payload->len);
}

enum {
    TYPE_VOID_RET_I32 = 0,
    TYPE_I32_RET_I32 = 1,
    TYPE_I32_I32_RET_I32 = 2,
    TYPE_I32_RET_VOID = 3,
    TYPE_VOID_RET_VOID = 4,
    TYPE_I32_I32_I32_RET_I32 = 5,
    TYPE_I32_I32_I32_RET_VOID = 6,
    TYPE_I32_I32_I32_I32_RET_I32 = 7,
    TYPE_COUNT = 8
};

static bool emit_i32_const(Buf *body, int32_t v) {
    return buf_put_u8(body, WASM_OP_I32_CONST) && buf_put_sleb32(body, v);
}

static bool emit_local_get(Buf *body, uint32_t idx) {
    return buf_put_u8(body, WASM_OP_LOCAL_GET) && buf_put_uleb32(body, idx);
}

static bool emit_local_set(Buf *body, uint32_t idx) {
    return buf_put_u8(body, WASM_OP_LOCAL_SET) && buf_put_uleb32(body, idx);
}

static bool emit_global_get(Buf *body, uint32_t idx) {
    return buf_put_u8(body, WASM_OP_GLOBAL_GET) && buf_put_uleb32(body, idx);
}

static bool emit_global_set(Buf *body, uint32_t idx) {
    return buf_put_u8(body, WASM_OP_GLOBAL_SET) && buf_put_uleb32(body, idx);
}

static bool emit_call(Buf *body, uint32_t fn_idx) {
    return buf_put_u8(body, WASM_OP_CALL) && buf_put_uleb32(body, fn_idx);
}

static bool emit_i32_load(Buf *body, uint32_t align, uint32_t offset) {
    return buf_put_u8(body, WASM_OP_I32_LOAD) &&
           buf_put_uleb32(body, align) &&
           buf_put_uleb32(body, offset);
}

static bool emit_i32_store(Buf *body, uint32_t align, uint32_t offset) {
    return buf_put_u8(body, WASM_OP_I32_STORE) &&
           buf_put_uleb32(body, align) &&
           buf_put_uleb32(body, offset);
}

static bool emit_stack_addr_from_sp_delta(Buf *body, int32_t delta) {
    bool ok = true;
    ok = ok && emit_global_get(body, G_SP);
    if (delta != 0) ok = ok && emit_i32_const(body, delta) && buf_put_u8(body, WASM_OP_I32_ADD);
    ok = ok && emit_i32_const(body, 4) && buf_put_u8(body, WASM_OP_I32_MUL);
    if (STACK_BASE_BYTES != 0u) {
        ok = ok && emit_i32_const(body, (int32_t)STACK_BASE_BYTES) && buf_put_u8(body, WASM_OP_I32_ADD);
    }
    return ok;
}

static bool emit_stack_addr_from_local_index(Buf *body, uint32_t local_idx) {
    bool ok = true;
    ok = ok && emit_local_get(body, local_idx);
    ok = ok && emit_i32_const(body, 4) && buf_put_u8(body, WASM_OP_I32_MUL);
    if (STACK_BASE_BYTES != 0u) {
        ok = ok && emit_i32_const(body, (int32_t)STACK_BASE_BYTES) && buf_put_u8(body, WASM_OP_I32_ADD);
    }
    return ok;
}

static bool emit_push_local(Buf *body, uint32_t local_idx) {
    bool ok = true;
    ok = ok && emit_stack_addr_from_sp_delta(body, 0);
    ok = ok && emit_local_get(body, local_idx);
    ok = ok && emit_i32_store(body, 2u, 0u);
    ok = ok && emit_global_get(body, G_SP);
    ok = ok && emit_i32_const(body, 1) && buf_put_u8(body, WASM_OP_I32_ADD);
    ok = ok && emit_global_set(body, G_SP);
    return ok;
}

static bool emit_pop_to_local(Buf *body, uint32_t local_idx) {
    bool ok = true;
    ok = ok && emit_global_get(body, G_SP);
    ok = ok && emit_i32_const(body, 1) && buf_put_u8(body, WASM_OP_I32_SUB);
    ok = ok && emit_global_set(body, G_SP);
    ok = ok && emit_stack_addr_from_sp_delta(body, 0);
    ok = ok && emit_i32_load(body, 2u, 0u);
    ok = ok && emit_local_set(body, local_idx);
    return ok;
}

static bool emit_peek_to_local(Buf *body, uint32_t depth, uint32_t local_idx) {
    int32_t delta = -(int32_t)(depth + 1u);
    bool ok = true;
    ok = ok && emit_stack_addr_from_sp_delta(body, delta);
    ok = ok && emit_i32_load(body, 2u, 0u);
    ok = ok && emit_local_set(body, local_idx);
    return ok;
}

static bool emit_call_addr_from_call_sp_delta(Buf *body, int32_t delta, uint32_t out_local_idx) {
    bool ok = true;
    ok = ok && emit_global_get(body, G_CALL_SP);
    if (delta != 0) ok = ok && emit_i32_const(body, delta) && buf_put_u8(body, WASM_OP_I32_ADD);
    ok = ok && emit_i32_const(body, (int32_t)CALL_ENTRY_BYTES) && buf_put_u8(body, WASM_OP_I32_MUL);
    ok = ok && emit_i32_const(body, (int32_t)CALL_BASE_BYTES) && buf_put_u8(body, WASM_OP_I32_ADD);
    ok = ok && emit_local_set(body, out_local_idx);
    return ok;
}

static bool emit_set_pc_next_and_loop(Buf *body, uint32_t next_idx) {
    return emit_i32_const(body, (int32_t)next_idx) &&
           emit_global_set(body, G_PC) &&
           buf_put_u8(body, WASM_OP_BR) &&
           buf_put_uleb32(body, 1u);
}

static bool emit_type_section(Buf *payload) {
    /* Types:
     * 0: () -> i32
     * 1: (i32) -> i32
     * 2: (i32, i32) -> i32
     * 3: (i32) -> ()
     * 4: () -> ()
     * 5: (i32, i32, i32) -> i32
     * 6: (i32, i32, i32) -> ()
     * 7: (i32, i32, i32, i32) -> i32
     */
    if (!buf_put_uleb32(payload, TYPE_COUNT)) return false;

    /* 0 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 0u)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;

    /* 1 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;

    /* 2 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 2u)) return false;
    if (!buf_put_u8(payload, WASM_VALTYPE_I32) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;

    /* 3 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 0u)) return false;

    /* 4 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 0u)) return false;
    if (!buf_put_uleb32(payload, 0u)) return false;

    /* 5 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 3u)) return false;
    if (!buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;

    /* 6 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 3u)) return false;
    if (!buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 0u)) return false;

    /* 7 */
    if (!buf_put_u8(payload, 0x60u)) return false;
    if (!buf_put_uleb32(payload, 4u)) return false;
    if (!buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32) ||
        !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
    if (!buf_put_uleb32(payload, 1u) || !buf_put_u8(payload, WASM_VALTYPE_I32)) return false;

    return true;
}

static bool emit_import_func(Buf *payload, const char *name, uint32_t type_idx) {
    return buf_put_name(payload, "env") &&
           buf_put_name(payload, name) &&
           buf_put_u8(payload, 0x00u) &&
           buf_put_uleb32(payload, type_idx);
}

static bool emit_import_section(Buf *payload) {
    static const struct {
        const char *name;
        uint32_t type_idx;
    } imports[IMP_COUNT] = {
        {"mot_input_props", TYPE_VOID_RET_I32},
        {"mot_input_children", TYPE_VOID_RET_I32},
        {"mot_const", TYPE_I32_RET_I32},
        {"mot_null", TYPE_VOID_RET_I32},
        {"mot_true", TYPE_VOID_RET_I32},
        {"mot_false", TYPE_VOID_RET_I32},
        {"mot_int", TYPE_I32_RET_I32},
        {"mot_load_global", TYPE_I32_RET_I32},
        {"mot_is_truthy", TYPE_I32_RET_I32},
        {"mot_unary", TYPE_I32_I32_RET_I32},
        {"mot_binary", TYPE_I32_I32_I32_RET_I32},
        {"mot_load_field", TYPE_I32_I32_RET_I32},
        {"mot_load_index", TYPE_I32_I32_RET_I32},
        {"mot_store_field", TYPE_I32_I32_I32_RET_VOID},
        {"mot_store_index", TYPE_I32_I32_I32_RET_VOID},
        {"mot_emit_literal", TYPE_I32_RET_VOID},
        {"mot_emit_text", TYPE_I32_RET_VOID},
        {"mot_emit_raw", TYPE_I32_RET_VOID},
        {"mot_emit_attr_start", TYPE_I32_RET_VOID},
        {"mot_emit_attr_end", TYPE_VOID_RET_VOID},
        {"mot_emit_tag_open", TYPE_I32_RET_VOID},
        {"mot_emit_tag_end", TYPE_VOID_RET_VOID},
        {"mot_emit_tag_close", TYPE_I32_RET_VOID},
        {"mot_emit_tag_self", TYPE_VOID_RET_VOID},
        {"mot_iter_start", TYPE_I32_RET_I32},
        {"mot_iter_has_next", TYPE_I32_RET_I32},
        {"mot_iter_next_value", TYPE_I32_RET_I32},
        {"mot_fetch_data", TYPE_I32_I32_I32_RET_I32},
        {"mot_fetch_data_result", TYPE_VOID_RET_I32},
        {"mot_call_builtin", TYPE_I32_I32_I32_I32_RET_I32},
        {"mot_component_load", TYPE_I32_I32_I32_I32_RET_I32},
        {"mot_component_linked", TYPE_I32_I32_I32_I32_RET_I32},
        {"mot_dep_start", TYPE_I32_RET_VOID},
        {"mot_dep_end", TYPE_VOID_RET_VOID},
        {"mot_array_new", TYPE_I32_I32_I32_RET_I32},
        {"mot_concat", TYPE_I32_I32_I32_RET_I32},
        {"mot_obj_new", TYPE_VOID_RET_I32},
        {"mot_obj_set", TYPE_I32_I32_I32_RET_VOID},
        {"mot_slot_start", TYPE_I32_RET_VOID},
        {"mot_slot_end", TYPE_VOID_RET_I32},
        {"mot_slot_default", TYPE_I32_RET_VOID},
    };
    uint32_t i;
    if (!buf_put_uleb32(payload, IMP_COUNT)) return false;
    for (i = 0; i < IMP_COUNT; i++) {
        if (!emit_import_func(payload, imports[i].name, imports[i].type_idx)) return false;
    }
    return true;
}

static bool emit_function_section(Buf *payload) {
    if (!buf_put_uleb32(payload, 1u)) return false;
    if (!buf_put_uleb32(payload, TYPE_VOID_RET_I32)) return false; /* mot_component_run: () -> i32 */
    return true;
}

static bool emit_memory_section(Buf *payload) {
    if (!buf_put_uleb32(payload, 1u)) return false;
    if (!buf_put_u8(payload, 0x00u)) return false;      /* flags: min only */
    if (!buf_put_uleb32(payload, 2u)) return false;     /* 2 pages */
    return true;
}

static bool emit_global_section(Buf *payload) {
    uint32_t i;
    if (!buf_put_uleb32(payload, G_COUNT)) return false;
    for (i = 0; i < G_COUNT; i++) {
        if (!buf_put_u8(payload, WASM_VALTYPE_I32)) return false;
        if (!buf_put_u8(payload, 0x01u)) return false; /* mutable */
        if (!emit_i32_const(payload, 0)) return false;
        if (!buf_put_u8(payload, WASM_OP_END)) return false;
    }
    return true;
}

static bool emit_export_section(Buf *payload) {
    uint32_t run_fn_index = IMP_COUNT; /* first defined func after imports */
    if (!buf_put_uleb32(payload, 2u)) return false;

    if (!buf_put_name(payload, "mot_component_run")) return false;
    if (!buf_put_u8(payload, 0x00u)) return false; /* func */
    if (!buf_put_uleb32(payload, run_fn_index)) return false;

    if (!buf_put_name(payload, "memory")) return false;
    if (!buf_put_u8(payload, 0x02u)) return false; /* memory */
    if (!buf_put_uleb32(payload, 0u)) return false;
    return true;
}

static bool emit_restore_frame_and_loop(Buf *body) {
    bool ok = true;
    /* call_sp-- */
    ok = ok && emit_global_get(body, G_CALL_SP);
    ok = ok && emit_i32_const(body, 1) && buf_put_u8(body, WASM_OP_I32_SUB);
    ok = ok && emit_global_set(body, G_CALL_SP);

    /* local2 = call frame base address */
    ok = ok && emit_call_addr_from_call_sp_delta(body, 0, 2u);

    /* pc = frame.return_pc */
    ok = ok && emit_local_get(body, 2u);
    ok = ok && emit_i32_load(body, 2u, 0u);
    ok = ok && emit_global_set(body, G_PC);

    /* code_end = frame.code_end */
    ok = ok && emit_local_get(body, 2u);
    ok = ok && emit_i32_load(body, 2u, 4u);
    ok = ok && emit_global_set(body, G_CODE_END);

    /* frame_base = frame.frame_base */
    ok = ok && emit_local_get(body, 2u);
    ok = ok && emit_i32_load(body, 2u, 8u);
    ok = ok && emit_global_set(body, G_FRAME_BASE);

    /* sp = frame.stack_sp */
    ok = ok && emit_local_get(body, 2u);
    ok = ok && emit_i32_load(body, 2u, 12u);
    ok = ok && emit_global_set(body, G_SP);

    /* continue main loop */
    ok = ok && buf_put_u8(body, WASM_OP_BR) && buf_put_uleb32(body, 0u);
    return ok;
}

static bool emit_code_section(Buf *payload,
                              const BytecodeModule *mod,
                              const InstructionVec *instructions,
                              const FunctionRange *functions,
                              uint32_t function_count,
                              char *error_buf,
                              size_t error_buf_len) {
    Buf body = {0};
    bool ok = true;
    uint32_t i;

    (void)mod;
    if (function_count == 0u) return false;
    if (!buf_put_uleb32(payload, 1u)) return false; /* one function body */

    /* locals: 3 i32 temps (local0/local1/local2) */
    ok = ok && buf_put_uleb32(&body, 1u);
    ok = ok && buf_put_uleb32(&body, 3u);
    ok = ok && buf_put_u8(&body, WASM_VALTYPE_I32);

    /* init on first entry */
    ok = ok && emit_global_get(&body, G_INITIALIZED);
    ok = ok && buf_put_u8(&body, WASM_OP_I32_EQZ);
    ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
    ok = ok && emit_i32_const(&body, 0) && emit_global_set(&body, G_SP);
    ok = ok && emit_i32_const(&body, 0) && emit_global_set(&body, G_CALL_SP);
    ok = ok && emit_i32_const(&body, 0) && emit_global_set(&body, G_FRAME_BASE);
    ok = ok && emit_i32_const(&body, (int32_t)functions[0].start) && emit_global_set(&body, G_PC);
    ok = ok && emit_i32_const(&body, (int32_t)functions[0].end) && emit_global_set(&body, G_CODE_END);

    ok = ok && emit_call(&body, IMP_INPUT_PROPS);
    ok = ok && emit_local_set(&body, 0u);
    ok = ok && emit_push_local(&body, 0u);

    ok = ok && emit_call(&body, IMP_INPUT_CHILDREN);
    ok = ok && emit_local_set(&body, 0u);
    ok = ok && emit_push_local(&body, 0u);

    ok = ok && emit_i32_const(&body, 1) && emit_global_set(&body, G_INITIALIZED);
    ok = ok && buf_put_u8(&body, WASM_OP_END);

    /* main dispatch loop */
    ok = ok && buf_put_u8(&body, WASM_OP_LOOP) && buf_put_u8(&body, 0x40u);

    /* if (pc >= code_end) */
    ok = ok && emit_global_get(&body, G_PC);
    ok = ok && emit_global_get(&body, G_CODE_END);
    ok = ok && buf_put_u8(&body, WASM_OP_I32_GE_U);
    ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
    ok = ok && emit_global_get(&body, G_CALL_SP);
    ok = ok && buf_put_u8(&body, WASM_OP_I32_EQZ);
    ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
    ok = ok && emit_i32_const(&body, MOT_OK);
    ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
    ok = ok && buf_put_u8(&body, WASM_OP_END);
    ok = ok && emit_restore_frame_and_loop(&body);
    ok = ok && buf_put_u8(&body, WASM_OP_END);

    for (i = 0; ok && i < instructions->count; i++) {
        const NativeInstruction *inst = &instructions->items[i];
        ok = ok && emit_global_get(&body, G_PC);
        ok = ok && emit_i32_const(&body, (int32_t)i);
        ok = ok && buf_put_u8(&body, WASM_OP_I32_EQ);
        ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);

        switch (inst->opcode) {
            case BC_NOP:
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_CONST:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_CONST);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_POP:
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, 1);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_DUP:
                ok = ok && emit_peek_to_local(&body, 0u, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_LOAD:
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, inst->a) && buf_put_u8(&body, WASM_OP_I32_ADD);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_stack_addr_from_local_index(&body, 0u);
                ok = ok && emit_i32_load(&body, 2u, 0u);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_STORE:
                ok = ok && emit_peek_to_local(&body, 0u, 0u);
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, inst->a) && buf_put_u8(&body, WASM_OP_I32_ADD);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_stack_addr_from_local_index(&body, 1u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_store(&body, 2u, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_LOAD_GLOBAL:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_LOAD_GLOBAL);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_LOAD_FIELD:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_LOAD_FIELD);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_LOAD_INDEX:
                ok = ok && emit_pop_to_local(&body, 0u); /* index */
                ok = ok && emit_pop_to_local(&body, 1u); /* array */
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_LOAD_INDEX);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_STORE_FIELD:
                ok = ok && emit_pop_to_local(&body, 0u); /* object */
                ok = ok && emit_pop_to_local(&body, 1u); /* value */
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_call(&body, IMP_STORE_FIELD);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_STORE_INDEX:
                ok = ok && emit_pop_to_local(&body, 0u); /* index */
                ok = ok && emit_pop_to_local(&body, 1u); /* array */
                ok = ok && emit_pop_to_local(&body, 2u); /* value */
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_call(&body, IMP_STORE_INDEX);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_NULL:
                ok = ok && emit_call(&body, IMP_NULL);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_TRUE:
                ok = ok && emit_call(&body, IMP_TRUE);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_FALSE:
                ok = ok && emit_call(&body, IMP_FALSE);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_INT:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_INT);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_ADD:
            case BC_SUB:
            case BC_MUL:
            case BC_DIV:
            case BC_MOD:
            case BC_EQ:
            case BC_NEQ:
            case BC_LT:
            case BC_LTE:
            case BC_GT:
            case BC_GTE:
            case BC_AND:
            case BC_OR: {
                int32_t op_id = 0;
                switch (inst->opcode) {
                    case BC_ADD: op_id = NATIVE_BINARY_ADD; break;
                    case BC_SUB: op_id = NATIVE_BINARY_SUB; break;
                    case BC_MUL: op_id = NATIVE_BINARY_MUL; break;
                    case BC_DIV: op_id = NATIVE_BINARY_DIV; break;
                    case BC_MOD: op_id = NATIVE_BINARY_MOD; break;
                    case BC_EQ: op_id = NATIVE_BINARY_EQ; break;
                    case BC_NEQ: op_id = NATIVE_BINARY_NEQ; break;
                    case BC_LT: op_id = NATIVE_BINARY_LT; break;
                    case BC_LTE: op_id = NATIVE_BINARY_LTE; break;
                    case BC_GT: op_id = NATIVE_BINARY_GT; break;
                    case BC_GTE: op_id = NATIVE_BINARY_GTE; break;
                    case BC_AND: op_id = NATIVE_BINARY_AND; break;
                    case BC_OR: op_id = NATIVE_BINARY_OR; break;
                    default: break;
                }
                ok = ok && emit_pop_to_local(&body, 0u); /* b */
                ok = ok && emit_pop_to_local(&body, 1u); /* a */
                ok = ok && emit_i32_const(&body, op_id);
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_BINARY);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            }
            case BC_NEG:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_i32_const(&body, NATIVE_UNARY_NEG);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_UNARY);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_NOT:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_i32_const(&body, NATIVE_UNARY_NOT);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_UNARY);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_JUMP:
                ok = ok && emit_i32_const(&body, inst->a) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_JUMP_IF_FALSE:
                ok = ok && emit_peek_to_local(&body, 0u, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_IS_TRUTHY);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_EQZ);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, inst->a) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_ELSE);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u)) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_END);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_JUMP_IF_TRUE:
                ok = ok && emit_peek_to_local(&body, 0u, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_IS_TRUTHY);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, inst->a) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_ELSE);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u)) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_END);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_ITER_START:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_ITER_START);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_ITER_NEXT:
                ok = ok && emit_peek_to_local(&body, 0u, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_ITER_HAS_NEXT);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_ITER_NEXT_VALUE);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u)) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_ELSE);
                ok = ok && emit_i32_const(&body, inst->a) && emit_global_set(&body, G_PC);
                ok = ok && buf_put_u8(&body, WASM_OP_END);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_ITER_END:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_LITERAL:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_EMIT_LITERAL);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_TEXT:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_EMIT_TEXT);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_RAW:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_EMIT_RAW);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_ATTR_START:
                ok = ok && emit_i32_const(&body, inst->a) && emit_call(&body, IMP_EMIT_ATTR_START);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_ATTR_END:
                ok = ok && emit_call(&body, IMP_EMIT_ATTR_END);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_TAG_OPEN:
                ok = ok && emit_i32_const(&body, inst->a) && emit_call(&body, IMP_EMIT_TAG_OPEN);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_TAG_END:
                ok = ok && emit_call(&body, IMP_EMIT_TAG_END);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_TAG_CLOSE:
                ok = ok && emit_i32_const(&body, inst->a) && emit_call(&body, IMP_EMIT_TAG_CLOSE);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_EMIT_TAG_SELF:
                ok = ok && emit_call(&body, IMP_EMIT_TAG_SELF);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_FETCH_DATA:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_FETCH_DATA);
                ok = ok && emit_local_set(&body, 0u);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 1);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_EQ);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, (int32_t)i) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, MOT_AWAIT);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 0);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_LT_S);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, MOT_ERROR);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_call(&body, IMP_FETCH_DATA_RESULT);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_FETCH_WAIT:
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_CALL:
                if (inst->a < 0 || (uint32_t)inst->a >= function_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_CALL function index");
                    ok = false;
                    break;
                }
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_local_set(&body, 1u); /* callee frame_base / return stack_sp */
                ok = ok && emit_call_addr_from_call_sp_delta(&body, 0, 2u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u));
                ok = ok && emit_i32_store(&body, 2u, 0u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_CODE_END);
                ok = ok && emit_i32_store(&body, 2u, 4u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_store(&body, 2u, 8u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_i32_store(&body, 2u, 12u);
                ok = ok && emit_global_get(&body, G_CALL_SP);
                ok = ok && emit_i32_const(&body, 1) && buf_put_u8(&body, WASM_OP_I32_ADD);
                ok = ok && emit_global_set(&body, G_CALL_SP);
                ok = ok && emit_local_get(&body, 1u) && emit_global_set(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].start) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].end) && emit_global_set(&body, G_CODE_END);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_CALL_BUILTIN:
                if (inst->a < 0 || (uint32_t)inst->a >= mod->builtin_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_CALL_BUILTIN builtin index");
                    ok = false;
                    break;
                }
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_CALL_BUILTIN);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_CALL_PIPE:
                if (inst->a < 0 || (uint32_t)inst->a >= function_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_CALL_PIPE function index");
                    ok = false;
                    break;
                }
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, 1);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_local_set(&body, 1u);
                ok = ok && emit_call_addr_from_call_sp_delta(&body, 0, 2u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u));
                ok = ok && emit_i32_store(&body, 2u, 0u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_CODE_END);
                ok = ok && emit_i32_store(&body, 2u, 4u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_store(&body, 2u, 8u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_i32_store(&body, 2u, 12u);
                ok = ok && emit_global_get(&body, G_CALL_SP);
                ok = ok && emit_i32_const(&body, 1) && buf_put_u8(&body, WASM_OP_I32_ADD);
                ok = ok && emit_global_set(&body, G_CALL_SP);
                ok = ok && emit_local_get(&body, 1u) && emit_global_set(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].start) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].end) && emit_global_set(&body, G_CODE_END);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_RETURN:
                ok = ok && emit_global_get(&body, G_CALL_SP);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_EQZ);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, MOT_OK);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);
                ok = ok && emit_restore_frame_and_loop(&body);
                break;
            case BC_COMPONENT_START:
                if (inst->a < 0 || (uint32_t)inst->a >= function_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_COMPONENT_START function index");
                    ok = false;
                    break;
                }
                ok = ok && emit_pop_to_local(&body, 0u); /* children */
                ok = ok && emit_pop_to_local(&body, 1u); /* props */
                ok = ok && emit_call_addr_from_call_sp_delta(&body, 0, 2u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_i32_const(&body, (int32_t)(i + 1u));
                ok = ok && emit_i32_store(&body, 2u, 0u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_CODE_END);
                ok = ok && emit_i32_store(&body, 2u, 4u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_FRAME_BASE);
                ok = ok && emit_i32_store(&body, 2u, 8u);
                ok = ok && emit_local_get(&body, 2u);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_store(&body, 2u, 12u);
                ok = ok && emit_global_get(&body, G_CALL_SP);
                ok = ok && emit_i32_const(&body, 1) && buf_put_u8(&body, WASM_OP_I32_ADD);
                ok = ok && emit_global_set(&body, G_CALL_SP);
                ok = ok && emit_global_get(&body, G_SP) && emit_global_set(&body, G_FRAME_BASE);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].start) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, (int32_t)functions[inst->a].end) && emit_global_set(&body, G_CODE_END);
                ok = ok && emit_push_local(&body, 1u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && buf_put_u8(&body, WASM_OP_BR) && buf_put_uleb32(&body, 1u);
                break;
            case BC_COMPONENT_END:
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_COMPONENT_LOAD:
                if (inst->a < 0 || (uint32_t)inst->a >= mod->comp_ref_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_COMPONENT_LOAD component index");
                    ok = false;
                    break;
                }
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_COMPONENT_LOAD);
                ok = ok && emit_local_set(&body, 0u);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 1);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_EQ);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, (int32_t)i) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, MOT_AWAIT);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 0);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_LT_S);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, MOT_ERROR);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_COMPONENT_LINKED:
                if (inst->a < 0 || (uint32_t)inst->a >= mod->comp_ref_count) {
                    set_error(error_buf, error_buf_len, "invalid BC_COMPONENT_LINKED component index");
                    ok = false;
                    break;
                }
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_COMPONENT_LINKED);
                ok = ok && emit_local_set(&body, 0u);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 1);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_EQ);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, (int32_t)i) && emit_global_set(&body, G_PC);
                ok = ok && emit_i32_const(&body, MOT_AWAIT);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_i32_const(&body, 0);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_LT_S);
                ok = ok && buf_put_u8(&body, WASM_OP_IF) && buf_put_u8(&body, 0x40u);
                ok = ok && emit_i32_const(&body, MOT_ERROR);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                ok = ok && buf_put_u8(&body, WASM_OP_END);

                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->b);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_SLOT_START:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_SLOT_START);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_SLOT_END:
                ok = ok && emit_call(&body, IMP_SLOT_END);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_SLOT_DEFAULT:
                ok = ok && emit_pop_to_local(&body, 0u);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_SLOT_DEFAULT);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_DEP_START:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_call(&body, IMP_DEP_START);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_DEP_END:
                ok = ok && emit_call(&body, IMP_DEP_END);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_ARRAY_NEW:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_ARRAY_NEW);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_OBJECT_NEW:
                ok = ok && emit_call(&body, IMP_OBJ_NEW);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_OBJECT_SET:
                ok = ok && emit_pop_to_local(&body, 0u);     /* value */
                ok = ok && emit_peek_to_local(&body, 0u, 1u);/* object */
                ok = ok && emit_local_get(&body, 1u);
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_local_get(&body, 0u);
                ok = ok && emit_call(&body, IMP_OBJ_SET);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_CONCAT:
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, (int32_t)STACK_BASE_BYTES);
                ok = ok && emit_call(&body, IMP_CONCAT);
                ok = ok && emit_local_set(&body, 0u);
                ok = ok && emit_global_get(&body, G_SP);
                ok = ok && emit_i32_const(&body, inst->a);
                ok = ok && buf_put_u8(&body, WASM_OP_I32_SUB);
                ok = ok && emit_global_set(&body, G_SP);
                ok = ok && emit_push_local(&body, 0u);
                ok = ok && emit_set_pc_next_and_loop(&body, i + 1u);
                break;
            case BC_HALT:
                ok = ok && emit_i32_const(&body, MOT_OK);
                ok = ok && buf_put_u8(&body, WASM_OP_RETURN);
                break;
            default:
                set_error(error_buf, error_buf_len, "unsupported opcode in wasm lowerer");
                ok = false;
                break;
        }

        ok = ok && buf_put_u8(&body, WASM_OP_END);
    }

    /* default: invalid pc => error */
    ok = ok && emit_i32_const(&body, MOT_ERROR);
    ok = ok && buf_put_u8(&body, WASM_OP_RETURN);

    ok = ok && buf_put_u8(&body, WASM_OP_END); /* loop */
    ok = ok && emit_i32_const(&body, MOT_ERROR);
    ok = ok && buf_put_u8(&body, WASM_OP_END); /* function */

    if (!ok) {
        free(body.data);
        return false;
    }

    if (!buf_put_uleb32(payload, (uint32_t)body.len)) {
        free(body.data);
        return false;
    }
    if (!buf_put_bytes(payload, body.data, body.len)) {
        free(body.data);
        return false;
    }
    free(body.data);
    return true;
}

static bool build_native_component_wasm(const BytecodeModule *mod,
                                        const InstructionVec *instructions,
                                        const FunctionRange *functions,
                                        uint32_t function_count,
                                        uint32_t main_end_index,
                                        uint8_t **out_wasm,
                                        size_t *out_wasm_len,
                                        char *error_buf,
                                        size_t error_buf_len) {
    static const uint8_t wasm_header[8] = {0x00u, 0x61u, 0x73u, 0x6du, 0x01u, 0x00u, 0x00u, 0x00u};
    const char *meta_section_name = "mot.native_meta";
    Buf wasm = {0};
    Buf custom_payload = {0};
    Buf type_payload = {0};
    Buf import_payload = {0};
    Buf function_payload = {0};
    Buf memory_payload = {0};
    Buf global_payload = {0};
    Buf export_payload = {0};
    Buf code_payload = {0};
    bool ok = true;

    if (instructions->count > 0x7fffffffu) {
        set_error(error_buf, error_buf_len, "instruction count exceeds i32 limits");
        return false;
    }

    ok = ok && buf_put_bytes(&wasm, wasm_header, sizeof(wasm_header));

    /* Custom section payload = name + meta bytes */
    ok = ok && buf_put_name(&custom_payload, meta_section_name);
    ok = ok && emit_native_meta_section(mod,
                                        functions,
                                        function_count,
                                        main_end_index,
                                        instructions->count,
                                        &custom_payload,
                                        error_buf,
                                        error_buf_len);
    ok = ok && append_wasm_section(&wasm, 0u, &custom_payload);

    ok = ok && emit_type_section(&type_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_TYPE, &type_payload);

    ok = ok && emit_import_section(&import_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_IMPORT, &import_payload);

    ok = ok && emit_function_section(&function_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_FUNCTION, &function_payload);

    ok = ok && emit_memory_section(&memory_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_MEMORY, &memory_payload);

    ok = ok && emit_global_section(&global_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_GLOBAL, &global_payload);

    ok = ok && emit_export_section(&export_payload);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_EXPORT, &export_payload);

    ok = ok && emit_code_section(&code_payload,
                                 mod,
                                 instructions,
                                 functions,
                                 function_count,
                                 error_buf,
                                 error_buf_len);
    ok = ok && append_wasm_section(&wasm, WASM_SECTION_CODE, &code_payload);

    free(custom_payload.data);
    free(type_payload.data);
    free(import_payload.data);
    free(function_payload.data);
    free(memory_payload.data);
    free(global_payload.data);
    free(export_payload.data);
    free(code_payload.data);

    if (!ok) {
        free(wasm.data);
        if (!error_buf || !error_buf[0]) {
            set_error(error_buf, error_buf_len, "failed to build native wasm module");
        }
        return false;
    }

    *out_wasm = wasm.data;
    *out_wasm_len = wasm.len;
    return true;
}

MotTranspilerStatus mot_transpile_component_bytecode_to_wasm(
    const uint8_t *bytecode,
    size_t bytecode_len,
    const MotBytecodeToWasmOptions *options,
    uint8_t **out_wasm,
    size_t *out_wasm_len,
    char *error_buf,
    size_t error_buf_len
) {
    Arena *arena = NULL;
    BytecodeModule *mod = NULL;
    InstructionVec instructions = {0};
    FunctionRange *function_ranges = NULL;
    uint32_t main_count = 0;
    uint32_t instr_base = 0;
    uint32_t i;
    bool ok = false;
    (void)options;

    if (out_wasm) *out_wasm = NULL;
    if (out_wasm_len) *out_wasm_len = 0;
    set_error(error_buf, error_buf_len, "");

    if (!bytecode || bytecode_len < 10u) {
        set_error(error_buf, error_buf_len, "bytecode buffer too short");
        return MOT_TRANSPILER_INVALID_INPUT;
    }
    if (!out_wasm || !out_wasm_len) {
        set_error(error_buf, error_buf_len, "out_wasm/out_wasm_len are required");
        return MOT_TRANSPILER_INTERNAL_ERROR;
    }
    if (bytecode_len > 0xffffffffu) {
        set_error(error_buf, error_buf_len, "bytecode exceeds 4GiB transpiler limit");
        return MOT_TRANSPILER_INVALID_INPUT;
    }

    arena = arena_create(64u * 1024u);
    if (!arena) {
        set_error(error_buf, error_buf_len, "arena_create failed");
        return MOT_TRANSPILER_OOM;
    }

    mod = bytecode_deserialize(bytecode, (uint32_t)bytecode_len, arena);
    if (!mod) {
        arena_destroy(arena);
        set_error(error_buf, error_buf_len, "bytecode_deserialize failed");
        return MOT_TRANSPILER_INVALID_INPUT;
    }
    if (mod->version_major != BYTECODE_VERSION_MAJOR || mod->version_minor != BYTECODE_VERSION_MINOR) {
        arena_destroy(arena);
        set_error(error_buf, error_buf_len, "unsupported bytecode version");
        return MOT_TRANSPILER_INVALID_INPUT;
    }
    if (mod->func_count == 0) {
        arena_destroy(arena);
        set_error(error_buf, error_buf_len, "component bytecode must include at least one function chunk");
        return MOT_TRANSPILER_INVALID_INPUT;
    }

    if (!decode_chunk_instructions(mod->main.code,
                                   mod->main.code_len,
                                   0u,
                                   &instructions,
                                   &main_count,
                                   error_buf,
                                   error_buf_len)) {
        arena_destroy(arena);
        free(instructions.items);
        return MOT_TRANSPILER_INVALID_INPUT;
    }

    instr_base = main_count;
    function_ranges = (FunctionRange *)calloc(mod->func_count, sizeof(FunctionRange));
    if (!function_ranges) {
        arena_destroy(arena);
        free(instructions.items);
        set_error(error_buf, error_buf_len, "out of memory allocating function ranges");
        return MOT_TRANSPILER_OOM;
    }

    for (i = 0; i < mod->func_count; i++) {
        uint32_t func_count = 0;
        function_ranges[i].start = instr_base;
        if (!decode_chunk_instructions(mod->functions[i].code,
                                       mod->functions[i].code_len,
                                       instr_base,
                                       &instructions,
                                       &func_count,
                                       error_buf,
                                       error_buf_len)) {
            free(function_ranges);
            free(instructions.items);
            arena_destroy(arena);
            return MOT_TRANSPILER_INVALID_INPUT;
        }
        instr_base += func_count;
        function_ranges[i].end = instr_base;
    }

    ok = build_native_component_wasm(mod,
                                     &instructions,
                                     function_ranges,
                                     mod->func_count,
                                     main_count,
                                     out_wasm,
                                     out_wasm_len,
                                     error_buf,
                                     error_buf_len);

    free(function_ranges);
    free(instructions.items);
    arena_destroy(arena);

    if (!ok) {
        if (!error_buf || !error_buf[0]) {
            set_error(error_buf, error_buf_len, "failed to emit native component wasm");
        }
        return MOT_TRANSPILER_INTERNAL_ERROR;
    }

    return MOT_TRANSPILER_OK;
}
