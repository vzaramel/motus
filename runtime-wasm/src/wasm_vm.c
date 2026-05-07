/*
 * Motus WASM Runtime Adapter
 *
 * Reuses the shared C VM (`src/runtime/vm.c`) and bytecode loader
 * (`src/compiler/bytecode.c`) instead of maintaining a separate interpreter.
 *
 * Build notes:
 * - Compiled freestanding for wasm32 (no libc).
 * - Provides minimal libc symbols used by shared core code.
 * - Implements arena allocator on top of a fixed linear-memory heap.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "runtime/vm.h"
#include "compiler/bytecode.h"
#include "util/arena.h"
#include "mot.h"

#define WASM_EXPORT __attribute__((visibility("default")))
#define WASM_IMPORT extern

/* Host imports */
WASM_IMPORT void host_output(const char *data, uint32_t len);
WASM_IMPORT void host_error(const char *msg, uint32_t len);
WASM_IMPORT void host_log(const char *msg, uint32_t len);
WASM_IMPORT void host_render_complete(void);
WASM_IMPORT void host_dep_start(const char *path, uint32_t len);
WASM_IMPORT void host_dep_end(void);
WASM_IMPORT void host_component_start(uint32_t func_idx);
WASM_IMPORT void host_component_end(uint32_t func_idx);
WASM_IMPORT void host_slot_default_start(uint32_t func_idx);
WASM_IMPORT void host_slot_default_end(uint32_t func_idx);
WASM_IMPORT void host_debug_step(uint32_t chunk_kind, uint32_t chunk_index, uint32_t pc,
                                 uint32_t opcode, uint32_t line, uint32_t column,
                                 const char *source_path, uint32_t source_len);
WASM_IMPORT void host_fetch_data(uint32_t req_id, uint32_t query_ref, uint32_t signature,
                                 const char *name, const char *params_json,
                                 uint32_t is_single);
WASM_IMPORT void host_load_component(uint32_t req_id, const char *name,
                                     const char *path, const char *props_json,
                                     const char *children_json);
WASM_IMPORT void host_load_component_linked(uint32_t req_id, const char *name,
                                            const char *path, const char *props_json,
                                            const char *children_json);

/* Runtime states */
#define STATE_IDLE 0
#define STATE_RUNNING 1
#define STATE_AWAITING 2
#define STATE_DONE 3
#define STATE_ERROR 4

#define MOT_OK 0
#define MOT_ERROR -1
#define MOT_AWAIT -2

/* Shared freestanding heap */
#define HEAP_SIZE (16u * 1024u * 1024u)
static uint8_t g_heap[HEAP_SIZE];
static uint32_t g_heap_pos = 0;

/* Runtime singletons */
static Arena *g_arena = NULL;
static BytecodeModule *g_module = NULL;
static VM *g_vm = NULL;
static int g_state = STATE_IDLE;
static uint32_t g_next_req_id = 1;
static bool g_emit_component_markers = false;
static bool g_emit_debug_steps = false;
static char g_debug_snapshot[98304];

#define PENDING_NONE 0u
#define PENDING_FETCH 1u
#define PENDING_COMPONENT 2u

typedef struct {
    uint8_t kind;
    uint32_t req_id;
    bool ready;

    uint32_t query_ref;
    uint32_t signature;
    uint32_t is_single;
    char name[128];
    char params_json[2048];

    char comp_name[128];
    char comp_path[256];
    char props_json[4096];
    char children_json[4096];

    char payload[65536];
    uint32_t payload_len;
} PendingRequest;

static PendingRequest g_pending = {0};

/* Minimal libc symbols used by shared core (vm.c / bytecode.c). */
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

/* Forward declarations */
static uint32_t align8(uint32_t n);
static void *heap_alloc(size_t size);
static int i64_to_buf(int64_t value, char *buf, int cap);
static int f64_to_buf(double value, char *buf, int cap);

/* --- Additional libc stubs needed by the compiler pipeline --- */

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen;
    if (!*needle) return (char *)haystack;
    nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int ispunct(int c) { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }

int abs(int x) { return x < 0 ? -x : x; }

double strtod(const char *nptr, char **endptr) {
    double result = 0.0;
    int sign = 1;
    const char *p = nptr;

    while (isspace(*p)) p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    while (isdigit(*p)) { result = result * 10.0 + (*p - '0'); p++; }

    if (*p == '.') {
        double frac = 0.1;
        p++;
        while (isdigit(*p)) { result += (*p - '0') * frac; frac *= 0.1; p++; }
    }

    if (*p == 'e' || *p == 'E') {
        int esign = 1;
        int exp = 0;
        double mult;
        int i;
        p++;
        if (*p == '-') { esign = -1; p++; }
        else if (*p == '+') p++;
        while (isdigit(*p)) { exp = exp * 10 + (*p - '0'); p++; }
        mult = 1.0;
        for (i = 0; i < exp; i++) mult *= 10.0;
        if (esign < 0) result /= mult;
        else result *= mult;
    }

    if (endptr) *endptr = (char *)p;
    return result * sign;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    long long result = 0;
    int sign = 1;
    const char *p = nptr;
    (void)base; /* only base-10 needed */

    while (isspace(*p)) p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;

    while (isdigit(*p)) { result = result * 10 + (*p - '0'); p++; }
    if (endptr) *endptr = (char *)p;
    return result * sign;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    unsigned long result = 0;
    const char *p = nptr;
    (void)base;

    while (isspace(*p)) p++;
    if (*p == '+') p++;

    while (isdigit(*p)) { result = result * 10 + (*p - '0'); p++; }
    if (endptr) *endptr = (char *)p;
    return result;
}

/* Minimal snprintf — handles %s, %d, %u, %zu, %ld, %lu, %c, %p, %%, %x, %02x, %f */
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    size_t pos = 0;
    char tmp[64];

    if (!buf || size == 0) return 0;

#define PUT(c) do { if (pos + 1 < size) buf[pos] = (c); pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt); fmt++; continue; }
        fmt++;

        /* Flags/width — skip for simplicity */
        int zero_pad = 0, width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Length modifiers */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') { is_long = 2; fmt++; } }
        else if (*fmt == 'z') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) { PUT(*s); s++; }
            break;
        }
        case 'd': case 'i': {
            long long v = is_long >= 2 ? __builtin_va_arg(ap, long long)
                        : is_long ? (long long)__builtin_va_arg(ap, long)
                        : (long long)__builtin_va_arg(ap, int);
            int n = i64_to_buf(v, tmp, sizeof(tmp));
            int pad = width > n ? width - n : 0;
            while (pad-- > 0) PUT(zero_pad ? '0' : ' ');
            for (int j = 0; j < n; j++) PUT(tmp[j]);
            break;
        }
        case 'u': {
            unsigned long long v = is_long >= 2 ? __builtin_va_arg(ap, unsigned long long)
                                 : is_long ? (unsigned long long)__builtin_va_arg(ap, unsigned long)
                                 : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            int n = i64_to_buf((int64_t)v, tmp, sizeof(tmp));
            int pad = width > n ? width - n : 0;
            while (pad-- > 0) PUT(zero_pad ? '0' : ' ');
            for (int j = 0; j < n; j++) PUT(tmp[j]);
            break;
        }
        case 'x': case 'X': {
            unsigned long v = is_long ? __builtin_va_arg(ap, unsigned long)
                                      : (unsigned long)__builtin_va_arg(ap, unsigned int);
            const char *hex = (*fmt == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            else { char r[16]; int ri = 0; while (v) { r[ri++] = hex[v & 0xf]; v >>= 4; } while (ri > 0) tmp[n++] = r[--ri]; }
            int pad = width > n ? width - n : 0;
            while (pad-- > 0) PUT(zero_pad ? '0' : ' ');
            for (int j = 0; j < n; j++) PUT(tmp[j]);
            break;
        }
        case 'f': {
            double v = __builtin_va_arg(ap, double);
            int n = f64_to_buf(v, tmp, sizeof(tmp));
            for (int j = 0; j < n; j++) PUT(tmp[j]);
            break;
        }
        case 'c': { char c = (char)__builtin_va_arg(ap, int); PUT(c); break; }
        case 'p': { __builtin_va_arg(ap, void*); PUT('0'); PUT('x'); PUT('?'); break; }
        case '%': { PUT('%'); break; }
        default: PUT('%'); PUT(*fmt); break;
        }
        fmt++;
    }
#undef PUT
    if (pos < size) buf[pos] = '\0';
    else if (size > 0) buf[size - 1] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7fffffff, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[2048];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) host_log(buf, (uint32_t)n);
    return n;
}

int fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    char buf[2048];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n > 0) host_log(buf, (uint32_t)n);
    return n;
}

size_t fwrite(const void *ptr, size_t size, size_t count, void *stream) {
    (void)stream;
    size_t total = size * count;
    if (total > 0 && ptr) host_output((const char *)ptr, (uint32_t)total);
    return count;
}

/* malloc/free/realloc/calloc backed by the bump allocator.
   free is a no-op — memory is reclaimed on mot_reset_alloc. */

void *malloc(size_t size) { return heap_alloc(size); }
void free(void *ptr) { (void)ptr; }
void *calloc(size_t count, size_t size) {
    void *p = heap_alloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void *realloc(void *ptr, size_t new_size) {
    /* Bump allocator: allocate new, copy old data.
       We don't know old size, so copy new_size bytes (may over-read but safe within heap). */
    void *new_ptr = heap_alloc(new_size);
    if (new_ptr && ptr) memcpy(new_ptr, ptr, new_size);
    return new_ptr;
}

static uint32_t align8(uint32_t n) {
    return (n + 7u) & ~7u;
}

static void *heap_alloc(size_t size) {
    uint32_t need = align8((uint32_t)size);
    if (need == 0 || g_heap_pos + need > HEAP_SIZE) return NULL;
    void *ptr = &g_heap[g_heap_pos];
    g_heap_pos += need;
    return ptr;
}

/* Exported allocator for host-side bytecode copy. */
WASM_EXPORT uint32_t mot_alloc(uint32_t size) {
    void *ptr = heap_alloc((size_t)size);
    if (!ptr) return 0;
    return (uint32_t)(uintptr_t)ptr;
}

WASM_EXPORT void mot_reset_alloc(void) {
    g_heap_pos = 0;
    g_arena = NULL;
    g_module = NULL;
    g_vm = NULL;
    g_state = STATE_IDLE;
    g_next_req_id = 1;
    g_emit_component_markers = false;
    g_emit_debug_steps = false;
    memset(&g_pending, 0, sizeof(g_pending));
}

/* Arena implementation backed by the same freestanding heap. */
static ArenaChunk *chunk_create(size_t size) {
    ArenaChunk *chunk = (ArenaChunk *)heap_alloc(sizeof(ArenaChunk) + size);
    if (!chunk) return NULL;
    chunk->next = NULL;
    chunk->size = size;
    chunk->used = 0;
    return chunk;
}

Arena *arena_create(size_t chunk_size) {
    Arena *arena;

    if (chunk_size < 4096) chunk_size = 4096;
    arena = (Arena *)heap_alloc(sizeof(Arena));
    if (!arena) return NULL;

    arena->chunk_size = chunk_size;
    arena->head = chunk_create(chunk_size);
    arena->current = arena->head;
    arena->total_allocated = 0;
    if (!arena->head) return NULL;
    return arena;
}

void arena_destroy(Arena *arena) {
    (void)arena;
    /* No-op in freestanding bump allocator mode. */
}

void *arena_alloc(Arena *arena, size_t size) {
    ArenaChunk *chunk;
    size_t aligned;
    size_t new_size;
    ArenaChunk *new_chunk;
    void *ptr;

    if (!arena || size == 0) return NULL;
    aligned = align8((uint32_t)size);
    chunk = arena->current;
    if (!chunk) return NULL;

    if (chunk->used + aligned > chunk->size) {
        new_size = arena->chunk_size;
        if (aligned > new_size) new_size = aligned;
        new_chunk = chunk_create(new_size);
        if (!new_chunk) return NULL;
        chunk->next = new_chunk;
        arena->current = new_chunk;
        chunk = new_chunk;
    }

    ptr = chunk->data + chunk->used;
    chunk->used += aligned;
    arena->total_allocated += aligned;
    return ptr;
}

void *arena_calloc(Arena *arena, size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = arena_alloc(arena, total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

char *arena_strndup(Arena *arena, const char *str, size_t n) {
    char *dup;
    if (!str) return NULL;
    dup = (char *)arena_alloc(arena, n + 1);
    if (!dup) return NULL;
    memcpy(dup, str, n);
    dup[n] = '\0';
    return dup;
}

char *arena_strdup(Arena *arena, const char *str) {
    if (!str) return NULL;
    return arena_strndup(arena, str, strlen(str));
}

void arena_reset(Arena *arena) {
    ArenaChunk *chunk;
    if (!arena) return;
    chunk = arena->head;
    while (chunk) {
        chunk->used = 0;
        chunk = chunk->next;
    }
    arena->current = arena->head;
    arena->total_allocated = 0;
}

size_t arena_total_allocated(Arena *arena) {
    if (!arena) return 0;
    return arena->total_allocated;
}

/* JSON helpers for fetch params. */
static int i64_to_buf(int64_t value, char *buf, int cap) {
    char tmp[32];
    int pos = 0;
    int neg = 0;
    uint64_t n;
    int out = 0;

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

    if (neg && out < cap - 1) buf[out++] = '-';
    while (pos > 0 && out < cap - 1) buf[out++] = tmp[--pos];
    buf[out] = '\0';
    return out;
}

static int f64_to_buf(double value, char *buf, int cap) {
    int out = 0;
    int64_t whole;
    uint32_t frac;
    uint32_t div;

    if (cap <= 0) return 0;

    if (value < 0.0) {
        if (out < cap - 1) buf[out++] = '-';
        value = -value;
    }

    whole = (int64_t)value;
    out += i64_to_buf(whole, buf + out, cap - out);
    if (out >= cap - 1) return out;

    frac = (uint32_t)((value - (double)whole) * 1000000.0 + 0.5);
    if (frac == 0) {
        return out;
    }

    buf[out++] = '.';
    div = 100000;
    while (div > 0 && out < cap - 1) {
        uint32_t digit = frac / div;
        frac %= div;
        buf[out++] = (char)('0' + digit);
        div /= 10;
    }

    /* Trim trailing zeros. */
    while (out > 0 && buf[out - 1] == '0') out--;
    if (out > 0 && buf[out - 1] == '.') out--;
    buf[out] = '\0';
    return out;
}

static void copy_str_trunc(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void pending_clear(void) {
    memset(&g_pending, 0, sizeof(g_pending));
}

static bool buf_put_char(char *buf, uint32_t cap, uint32_t *pos, char c) {
    if (*pos + 1 >= cap) return false;
    buf[(*pos)++] = c;
    buf[*pos] = '\0';
    return true;
}

static bool buf_put_bytes(char *buf, uint32_t cap, uint32_t *pos,
                          const char *data, uint32_t len) {
    if (*pos + len >= cap) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
    buf[*pos] = '\0';
    return true;
}

static bool buf_put_escaped(char *buf, uint32_t cap, uint32_t *pos,
                            const char *s, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            if (!buf_put_char(buf, cap, pos, '\\')) return false;
            if (!buf_put_char(buf, cap, pos, c)) return false;
        } else if (c == '\n') {
            if (!buf_put_bytes(buf, cap, pos, "\\n", 2)) return false;
        } else if (c == '\r') {
            if (!buf_put_bytes(buf, cap, pos, "\\r", 2)) return false;
        } else if (c == '\t') {
            if (!buf_put_bytes(buf, cap, pos, "\\t", 2)) return false;
        } else if ((unsigned char)c < 0x20) {
            if (!buf_put_char(buf, cap, pos, '?')) return false;
        } else {
            if (!buf_put_char(buf, cap, pos, c)) return false;
        }
    }
    return true;
}

static bool buf_put_value_json(char *buf, uint32_t cap, uint32_t *pos, const Value *v) {
    char num[64];
    int nlen;
    if (!v) return buf_put_bytes(buf, cap, pos, "null", 4);
    switch (v->type) {
        case VAL_NULL:
            return buf_put_bytes(buf, cap, pos, "null", 4);
        case VAL_BOOL:
            return v->as.boolean ? buf_put_bytes(buf, cap, pos, "true", 4)
                                 : buf_put_bytes(buf, cap, pos, "false", 5);
        case VAL_INT:
            nlen = i64_to_buf(v->as.integer, num, (int)sizeof(num));
            return buf_put_bytes(buf, cap, pos, num, (uint32_t)nlen);
        case VAL_NUMBER:
            nlen = f64_to_buf(v->as.number, num, (int)sizeof(num));
            return buf_put_bytes(buf, cap, pos, num, (uint32_t)nlen);
        case VAL_STRING:
            if (!buf_put_char(buf, cap, pos, '"')) return false;
            if (!buf_put_escaped(buf, cap, pos, v->as.string.data, v->as.string.length)) return false;
            return buf_put_char(buf, cap, pos, '"');
        case VAL_ARRAY: {
            if (!buf_put_char(buf, cap, pos, '[')) return false;
            for (uint32_t i = 0; i < v->as.array->count; i++) {
                if (i > 0 && !buf_put_char(buf, cap, pos, ',')) return false;
                if (!buf_put_value_json(buf, cap, pos, &v->as.array->elements[i])) return false;
            }
            return buf_put_char(buf, cap, pos, ']');
        }
        case VAL_OBJECT: {
            if (!buf_put_char(buf, cap, pos, '{')) return false;
            for (uint32_t i = 0; i < v->as.object->count; i++) {
                const ObjectField *f = &v->as.object->fields[i];
                if (i > 0 && !buf_put_char(buf, cap, pos, ',')) return false;
                if (!buf_put_char(buf, cap, pos, '"')) return false;
                if (!buf_put_escaped(buf, cap, pos, f->key, (uint32_t)strlen(f->key))) return false;
                if (!buf_put_bytes(buf, cap, pos, "\":", 2)) return false;
                if (!buf_put_value_json(buf, cap, pos, &f->value)) return false;
            }
            return buf_put_char(buf, cap, pos, '}');
        }
        default:
            return buf_put_bytes(buf, cap, pos, "null", 4);
    }
}

static bool buf_put_u32(char *buf, uint32_t cap, uint32_t *pos, uint32_t value) {
    char num[24];
    int nlen = i64_to_buf((int64_t)value, num, (int)sizeof(num));
    return buf_put_bytes(buf, cap, pos, num, (uint32_t)nlen);
}

static bool buf_put_cstr_json(char *buf, uint32_t cap, uint32_t *pos, const char *value) {
    const char *s = value ? value : "";
    if (!buf_put_char(buf, cap, pos, '"')) return false;
    if (!buf_put_escaped(buf, cap, pos, s, (uint32_t)strlen(s))) return false;
    return buf_put_char(buf, cap, pos, '"');
}

static const char *frame_kind_name(VMFrameKind kind) {
    switch (kind) {
        case VM_FRAME_MAIN: return "main";
        case VM_FRAME_FUNCTION: return "function";
        case VM_FRAME_COMPONENT: return "component";
        default: return "unknown";
    }
}

static const char *frame_source_path(const CallFrame *frame) {
    if (!frame || !g_module) return "";
    if (frame->kind != VM_FRAME_MAIN &&
        g_module->func_debug &&
        frame->func_idx < g_module->func_count) {
        const FunctionDebugRef *debug_ref = &g_module->func_debug[frame->func_idx];
        if (debug_ref->source_path && debug_ref->source_path_len > 0) {
            return debug_ref->source_path;
        }
    }
    return "";
}

static bool vm_execution_complete(void) {
    CallFrame *frame;
    uint8_t *code_start;
    uint8_t *code_end;
    if (!g_vm) return true;
    if (g_vm->frame_count <= 0) return true;
    frame = &g_vm->frames[g_vm->frame_count - 1];
    if (!frame->chunk || !frame->chunk->code) return true;
    code_start = frame->chunk->code;
    code_end = code_start + frame->chunk->code_len;
    if (frame->ip > code_start && frame->ip <= code_end && frame->ip[-1] == BC_HALT) {
        return true;
    }
    return false;
}

static uint32_t build_debug_snapshot_json(char *out, uint32_t out_cap) {
    uint32_t pos = 0;
    uint32_t frame_count = 0;
    uint32_t stack_depth = 0;
    bool done = false;

    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!g_vm) {
        if (out_cap > 3) {
            memcpy(out, "{}", 3);
            return 2;
        }
        return 0;
    }

    frame_count = g_vm->frame_count > 0 ? (uint32_t)g_vm->frame_count : 0;
    stack_depth = (uint32_t)(g_vm->stack_top - g_vm->stack);
    done = vm_execution_complete();

    if (!buf_put_char(out, out_cap, &pos, '{')) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, "\"state\":", 8)) return pos;
    if (!buf_put_u32(out, out_cap, &pos, (uint32_t)g_state)) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, ",\"done\":", 8)) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, done ? "true" : "false", done ? 4u : 5u)) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, ",\"frameCount\":", 14)) return pos;
    if (!buf_put_u32(out, out_cap, &pos, frame_count)) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, ",\"stackDepth\":", 14)) return pos;
    if (!buf_put_u32(out, out_cap, &pos, stack_depth)) return pos;
    if (!buf_put_bytes(out, out_cap, &pos, ",\"frames\":[", 11)) return pos;

    for (uint32_t i = 0; i < frame_count; i++) {
        CallFrame *frame = &g_vm->frames[i];
        Value *slot_start = frame->slots;
        Value *slot_end = (i + 1u < frame_count) ? g_vm->frames[i + 1u].slots : g_vm->stack_top;
        uint32_t slot_base = (uint32_t)(slot_start - g_vm->stack);
        uint32_t slot_count = 0;
        uint32_t pc = 0;
        uint32_t next_opcode = 0;
        const char *kind_name = frame_kind_name(frame->kind);
        const char *source_path = frame_source_path(frame);
        if (slot_end < slot_start) slot_end = slot_start;
        slot_count = (uint32_t)(slot_end - slot_start);
        if (frame->chunk && frame->chunk->code && frame->ip >= frame->chunk->code) {
            pc = (uint32_t)(frame->ip - frame->chunk->code);
            if (pc < frame->chunk->code_len) {
                next_opcode = frame->chunk->code[pc];
            } else if (pc > 0 && pc <= frame->chunk->code_len) {
                next_opcode = frame->chunk->code[pc - 1];
            }
        }

        if (i > 0 && !buf_put_char(out, out_cap, &pos, ',')) return pos;
        if (!buf_put_char(out, out_cap, &pos, '{')) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, "\"index\":", 8)) return pos;
        if (!buf_put_u32(out, out_cap, &pos, i)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"kind\":", 8)) return pos;
        if (!buf_put_cstr_json(out, out_cap, &pos, kind_name)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"funcIdx\":", 10)) return pos;
        if (!buf_put_u32(out, out_cap, &pos, (uint32_t)frame->func_idx)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"pc\":", 6)) return pos;
        if (!buf_put_u32(out, out_cap, &pos, pc)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"nextOpcode\":", 14)) return pos;
        if (!buf_put_u32(out, out_cap, &pos, next_opcode)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"sourcePath\":", 14)) return pos;
        if (!buf_put_cstr_json(out, out_cap, &pos, source_path)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"slotBase\":", 11)) return pos;
        if (!buf_put_u32(out, out_cap, &pos, slot_base)) return pos;
        if (!buf_put_bytes(out, out_cap, &pos, ",\"locals\":[", 11)) return pos;

        for (uint32_t j = 0; j < slot_count; j++) {
            if (j > 0 && !buf_put_char(out, out_cap, &pos, ',')) return pos;
            if (!buf_put_char(out, out_cap, &pos, '{')) return pos;
            if (!buf_put_bytes(out, out_cap, &pos, "\"slot\":", 7)) return pos;
            if (!buf_put_u32(out, out_cap, &pos, slot_base + j)) return pos;
            if (!buf_put_bytes(out, out_cap, &pos, ",\"value\":", 9)) return pos;
            if (!buf_put_value_json(out, out_cap, &pos, &slot_start[j])) return pos;
            if (!buf_put_char(out, out_cap, &pos, '}')) return pos;
        }

        if (!buf_put_char(out, out_cap, &pos, ']')) return pos;
        if (!buf_put_char(out, out_cap, &pos, '}')) return pos;
    }

    if (!buf_put_char(out, out_cap, &pos, ']')) return pos;
    if (!buf_put_char(out, out_cap, &pos, '}')) return pos;
    return pos;
}

static void build_params_json(const DataRequirement *req, const Value *frame_slots,
                              char *out, uint32_t out_cap) {
    uint32_t pos = 0;
    out[0] = '\0';
    if (!buf_put_char(out, out_cap, &pos, '{')) return;
    if (!req || req->param_count == 0) {
        (void)buf_put_char(out, out_cap, &pos, '}');
        return;
    }

    for (uint16_t i = 0; i < req->param_count; i++) {
        const char *pname = req->param_names ? req->param_names[i] : "";
        const Value *v = frame_slots ? &frame_slots[req->param_slots[i]] : NULL;

        if (i > 0 && !buf_put_char(out, out_cap, &pos, ',')) break;
        if (!buf_put_char(out, out_cap, &pos, '"')) break;
        if (!buf_put_escaped(out, out_cap, &pos, pname, (uint32_t)strlen(pname))) break;
        if (!buf_put_bytes(out, out_cap, &pos, "\":", 2)) break;
        if (!buf_put_value_json(out, out_cap, &pos, v)) break;
    }

    (void)buf_put_char(out, out_cap, &pos, '}');
}

typedef struct {
    const char *cur;
    const char *end;
} JsonParser;

static void jp_skip_ws(JsonParser *p) {
    while (p->cur < p->end &&
           (*p->cur == ' ' || *p->cur == '\t' || *p->cur == '\n' || *p->cur == '\r')) {
        p->cur++;
    }
}

static bool jp_parse_value(JsonParser *p, VM *vm, Value *out);

static bool jp_parse_string(JsonParser *p, VM *vm, Value *out) {
    const char *src;
    char *tmp;
    uint32_t len = 0;

    if (p->cur >= p->end || *p->cur != '"') return false;
    p->cur++;
    src = p->cur;
    tmp = (char *)arena_alloc(vm->arena, (size_t)(p->end - p->cur + 1));
    if (!tmp) return false;

    while (p->cur < p->end) {
        char ch = *p->cur++;
        if (ch == '"') {
            *out = val_string(vm, tmp, len);
            return true;
        }
        if (ch == '\\') {
            if (p->cur >= p->end) return false;
            ch = *p->cur++;
            switch (ch) {
                case '"': break;
                case '\\': break;
                case '/': break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case 'u': {
                    /* Minimal unicode handling: consume 4 hex chars, emit '?'. */
                    for (int i = 0; i < 4; i++) {
                        if (p->cur >= p->end) return false;
                        p->cur++;
                    }
                    ch = '?';
                    break;
                }
                default:
                    return false;
            }
        } else if ((unsigned char)ch < 0x20) {
            return false;
        }
        tmp[len++] = ch;
    }

    (void)src;
    return false;
}

static double pow10_i32(int exp) {
    double x = 1.0;
    if (exp >= 0) {
        while (exp-- > 0) x *= 10.0;
    } else {
        while (exp++ < 0) x /= 10.0;
    }
    return x;
}

static bool jp_parse_number(JsonParser *p, Value *out) {
    const char *s = p->cur;
    bool neg = false;
    bool is_float = false;
    uint64_t int_part = 0;
    double frac = 0.0;
    double frac_div = 1.0;
    int exp = 0;
    bool exp_neg = false;

    if (s < p->end && *s == '-') {
        neg = true;
        s++;
    }
    if (s >= p->end) return false;
    if (*s == '0') {
        s++;
    } else if (*s >= '1' && *s <= '9') {
        while (s < p->end && *s >= '0' && *s <= '9') {
            int_part = int_part * 10u + (uint64_t)(*s - '0');
            s++;
        }
    } else {
        return false;
    }

    if (s < p->end && *s == '.') {
        is_float = true;
        s++;
        if (s >= p->end || *s < '0' || *s > '9') return false;
        while (s < p->end && *s >= '0' && *s <= '9') {
            frac = frac * 10.0 + (double)(*s - '0');
            frac_div *= 10.0;
            s++;
        }
    }

    if (s < p->end && (*s == 'e' || *s == 'E')) {
        is_float = true;
        s++;
        if (s < p->end && (*s == '+' || *s == '-')) {
            exp_neg = (*s == '-');
            s++;
        }
        if (s >= p->end || *s < '0' || *s > '9') return false;
        while (s < p->end && *s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
    }

    p->cur = s;

    if (!is_float) {
        int64_t ival = (int64_t)int_part;
        if (neg) ival = -ival;
        *out = val_int(ival);
        return true;
    }

    double value = (double)int_part + (frac / frac_div);
    if (exp != 0) value *= pow10_i32(exp_neg ? -exp : exp);
    if (neg) value = -value;
    *out = val_number(value);
    return true;
}

static bool jp_parse_array(JsonParser *p, VM *vm, Value *out) {
    Value arr = val_array(vm, 4);
    if (p->cur >= p->end || *p->cur != '[') return false;
    p->cur++;
    jp_skip_ws(p);
    if (p->cur < p->end && *p->cur == ']') {
        p->cur++;
        *out = arr;
        return true;
    }
    while (p->cur < p->end) {
        Value elem = val_null();
        if (!jp_parse_value(p, vm, &elem)) return false;
        array_push(vm, arr.as.array, elem);
        jp_skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            jp_skip_ws(p);
            continue;
        }
        if (p->cur < p->end && *p->cur == ']') {
            p->cur++;
            *out = arr;
            return true;
        }
        return false;
    }
    return false;
}

static bool jp_parse_object(JsonParser *p, VM *vm, Value *out) {
    Value obj = val_object(vm, 4);
    if (p->cur >= p->end || *p->cur != '{') return false;
    p->cur++;
    jp_skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') {
        p->cur++;
        *out = obj;
        return true;
    }
    while (p->cur < p->end) {
        Value key = val_null();
        Value val = val_null();
        if (!jp_parse_string(p, vm, &key) || key.type != VAL_STRING) return false;
        jp_skip_ws(p);
        if (p->cur >= p->end || *p->cur != ':') return false;
        p->cur++;
        jp_skip_ws(p);
        if (!jp_parse_value(p, vm, &val)) return false;
        object_set(vm, obj.as.object, key.as.string.data, val);
        jp_skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            jp_skip_ws(p);
            continue;
        }
        if (p->cur < p->end && *p->cur == '}') {
            p->cur++;
            *out = obj;
            return true;
        }
        return false;
    }
    return false;
}

static bool jp_parse_value(JsonParser *p, VM *vm, Value *out) {
    jp_skip_ws(p);
    if (p->cur >= p->end) return false;

    if (*p->cur == '"') return jp_parse_string(p, vm, out);
    if (*p->cur == '{') return jp_parse_object(p, vm, out);
    if (*p->cur == '[') return jp_parse_array(p, vm, out);
    if (*p->cur == '-' || (*p->cur >= '0' && *p->cur <= '9')) {
        return jp_parse_number(p, out);
    }
    if ((p->end - p->cur) >= 4 && strncmp(p->cur, "true", 4) == 0) {
        p->cur += 4;
        *out = val_bool(true);
        return true;
    }
    if ((p->end - p->cur) >= 5 && strncmp(p->cur, "false", 5) == 0) {
        p->cur += 5;
        *out = val_bool(false);
        return true;
    }
    if ((p->end - p->cur) >= 4 && strncmp(p->cur, "null", 4) == 0) {
        p->cur += 4;
        *out = val_null();
        return true;
    }
    return false;
}

static bool json_to_value(VM *vm, const char *json, uint32_t len, Value *out) {
    JsonParser p;
    if (!vm || !json || !out) return false;
    p.cur = json;
    p.end = json + len;
    if (!jp_parse_value(&p, vm, out)) return false;
    jp_skip_ws(&p);
    return p.cur == p.end;
}

/* VM host adapters */
static void wasm_output(const char *data, uint32_t len, void *userdata) {
    (void)userdata;
    host_output(data, len);
}

static void emit_component_marker(uint16_t func_idx, bool is_start) {
    char marker[48];
    int n;
    uint32_t pos = 0;
    if (!g_emit_component_markers || !g_vm) return;

    memcpy(marker + pos, "<!--motc:", 9);
    pos += 9;
    n = i64_to_buf((int64_t)func_idx, marker + pos, (int)(sizeof(marker) - pos));
    if (n <= 0) return;
    pos += (uint32_t)n;
    if (is_start) {
        memcpy(marker + pos, ":start-->", 9);
        pos += 9;
    } else {
        memcpy(marker + pos, ":end-->", 7);
        pos += 7;
    }
    vm_emit_raw(g_vm, marker, pos);
}

static void emit_slot_marker(uint16_t func_idx, bool is_start) {
    char marker[56];
    int n;
    uint32_t pos = 0;
    if (!g_emit_component_markers || !g_vm) return;

    memcpy(marker + pos, "<!--motslot:", 12);
    pos += 12;
    n = i64_to_buf((int64_t)func_idx, marker + pos, (int)(sizeof(marker) - pos));
    if (n <= 0) return;
    pos += (uint32_t)n;
    if (is_start) {
        memcpy(marker + pos, ":start-->", 9);
        pos += 9;
    } else {
        memcpy(marker + pos, ":end-->", 7);
        pos += 7;
    }
    vm_emit_raw(g_vm, marker, pos);
}

static void wasm_component_start(uint16_t func_idx, void *userdata) {
    (void)userdata;
    emit_component_marker(func_idx, true);
    host_component_start((uint32_t)func_idx);
}

static void wasm_component_end(uint16_t func_idx, void *userdata) {
    (void)userdata;
    emit_component_marker(func_idx, false);
    host_component_end((uint32_t)func_idx);
}

static void wasm_slot_default_start(uint16_t func_idx, void *userdata) {
    (void)userdata;
    emit_slot_marker(func_idx, true);
    host_slot_default_start((uint32_t)func_idx);
}

static void wasm_slot_default_end(uint16_t func_idx, void *userdata) {
    (void)userdata;
    emit_slot_marker(func_idx, false);
    host_slot_default_end((uint32_t)func_idx);
}

static void wasm_debug_step(uint8_t chunk_kind,
                            uint16_t chunk_index,
                            uint32_t pc,
                            uint8_t opcode,
                            uint32_t line,
                            uint32_t column,
                            const char *source_path,
                            void *userdata) {
    (void)userdata;
    uint32_t source_len = 0;
    if (!g_emit_debug_steps) return;
    if (source_path) {
        source_len = (uint32_t)strlen(source_path);
    }
    host_debug_step((uint32_t)chunk_kind,
                    (uint32_t)chunk_index,
                    pc,
                    (uint32_t)opcode,
                    line,
                    column,
                    source_path ? source_path : "",
                    source_len);
}

static Value pending_fetch_result(void) {
    Value out = val_null();
    if (!g_pending.ready) return val_null();
    if (g_pending.payload_len == 0) {
        pending_clear();
        return val_null();
    }
    if (!json_to_value(g_vm, g_pending.payload, g_pending.payload_len, &out)) {
        vm_error(g_vm, "Invalid JSON fetch payload");
        pending_clear();
        return val_null();
    }
    pending_clear();
    vm_clear_await(g_vm);
    return out;
}

static Value pending_component_result(void) {
    Value out = val_string(g_vm, g_pending.payload, g_pending.payload_len);
    pending_clear();
    vm_clear_await(g_vm);
    return out;
}

static bool parse_json_arg_or_null(const char *json, uint32_t len, Value *out,
                                   const char *context_msg) {
    if (!out) return false;
    if (!json || len == 0) {
        *out = val_null();
        return true;
    }
    if (!json_to_value(g_vm, json, len, out)) {
        vm_error(g_vm, context_msg);
        return false;
    }
    return true;
}

static bool prepare_component_render(uint16_t func_idx, const Value *props,
                                     const Value *children) {
    Chunk *comp_chunk;
    CallFrame *frame;

    if (!g_vm || !g_module) return false;

    comp_chunk = bytecode_get_function(g_module, func_idx);
    if (!comp_chunk) {
        vm_error(g_vm, "Invalid component function index");
        return false;
    }

    /* Reset transient VM execution state while preserving configured callbacks. */
    g_vm->stack_top = g_vm->stack;
    g_vm->frame_count = 0;
    g_vm->capture_depth = 0;
    g_vm->awaiting = false;
    g_vm->had_error = false;
    g_vm->error_msg[0] = '\0';

    frame = &g_vm->frames[g_vm->frame_count++];
    frame->chunk = comp_chunk;
    frame->ip = comp_chunk->code;
    frame->slots = g_vm->stack;
    frame->kind = VM_FRAME_MAIN;
    frame->func_idx = func_idx;

    if ((size_t)(g_vm->stack_top - g_vm->stack) + 2 > VM_STACK_MAX) {
        vm_error(g_vm, "Stack overflow in component render setup");
        return false;
    }

    *g_vm->stack_top++ = props ? *props : val_null();
    *g_vm->stack_top++ = children ? *children : val_null();
    pending_clear();
    return true;
}

static Value wasm_fetch(const DataRequirement *req, const Value *frame_slots, void *userdata) {
    char params_json[2048];
    (void)userdata;

    if (!req || !g_vm) return val_null();

    if (g_pending.kind == PENDING_FETCH && g_pending.ready) {
        return pending_fetch_result();
    }

    if (g_pending.kind == PENDING_FETCH) {
        vm_request_await(g_vm);
        return val_null();
    }
    if (g_pending.kind != PENDING_NONE) {
        vm_error(g_vm, "Concurrent async operations are not supported");
        return val_null();
    }

    build_params_json(req, frame_slots, params_json, sizeof(params_json));

    g_pending.kind = PENDING_FETCH;
    g_pending.req_id = g_next_req_id++;
    g_pending.query_ref = req->query_ref;
    g_pending.signature = req->signature;
    g_pending.is_single = req->is_single ? 1u : 0u;
    copy_str_trunc(g_pending.name, (uint32_t)sizeof(g_pending.name),
                   req->name ? req->name : "");
    copy_str_trunc(g_pending.params_json, (uint32_t)sizeof(g_pending.params_json), params_json);

    host_fetch_data(g_pending.req_id, req->query_ref, req->signature,
                    req->name ? req->name : "",
                    params_json,
                    req->is_single ? 1u : 0u);
    vm_request_await(g_vm);
    return val_null();
}

static Value wasm_component_load_impl(const ComponentRef *ref, const Value *args,
                                      uint8_t argc, void *userdata,
                                      bool linked_opcode) {
    char props_json[4096];
    char children_json[4096];
    uint32_t ppos = 0;
    uint32_t cpos = 0;
    (void)userdata;

    if (!ref || !g_vm) return val_null();

    if (g_pending.kind == PENDING_COMPONENT && g_pending.ready) {
        return pending_component_result();
    }

    if (g_pending.kind == PENDING_COMPONENT) {
        vm_request_await(g_vm);
        return val_null();
    }
    if (g_pending.kind != PENDING_NONE) {
        vm_error(g_vm, "Concurrent async operations are not supported");
        return val_null();
    }

    props_json[0] = '\0';
    children_json[0] = '\0';

    if (!buf_put_value_json(props_json, (uint32_t)sizeof(props_json), &ppos,
                            argc > 0 ? &args[0] : NULL)) {
        copy_str_trunc(props_json, (uint32_t)sizeof(props_json), "null");
    }
    if (!buf_put_value_json(children_json, (uint32_t)sizeof(children_json), &cpos,
                            argc > 1 ? &args[1] : NULL)) {
        copy_str_trunc(children_json, (uint32_t)sizeof(children_json), "null");
    }

    g_pending.kind = PENDING_COMPONENT;
    g_pending.req_id = g_next_req_id++;
    copy_str_trunc(g_pending.comp_name, (uint32_t)sizeof(g_pending.comp_name),
                   ref->name ? ref->name : "");
    copy_str_trunc(g_pending.comp_path, (uint32_t)sizeof(g_pending.comp_path),
                   ref->path ? ref->path : "");
    copy_str_trunc(g_pending.props_json, (uint32_t)sizeof(g_pending.props_json), props_json);
    copy_str_trunc(g_pending.children_json, (uint32_t)sizeof(g_pending.children_json), children_json);

    if (linked_opcode) {
        host_load_component_linked(g_pending.req_id,
                                   ref->name ? ref->name : "",
                                   ref->path ? ref->path : "",
                                   props_json,
                                   children_json);
    } else {
        host_load_component(g_pending.req_id,
                            ref->name ? ref->name : "",
                            ref->path ? ref->path : "",
                            props_json,
                            children_json);
    }
    vm_request_await(g_vm);
    return val_null();
}

static Value wasm_component_load(const ComponentRef *ref, const Value *args,
                                 uint8_t argc, void *userdata) {
    return wasm_component_load_impl(ref, args, argc, userdata, false);
}

static Value wasm_component_linked_load(const ComponentRef *ref, const Value *args,
                                        uint8_t argc, void *userdata) {
    return wasm_component_load_impl(ref, args, argc, userdata, true);
}

WASM_EXPORT int mot_init(const uint8_t *bytecode, uint32_t len) {
    if (!bytecode || len == 0) {
        host_error("Empty bytecode", 13);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    g_arena = arena_create(64 * 1024);
    if (!g_arena) {
        host_error("Arena create failed", 17);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    g_module = bytecode_deserialize(bytecode, len, g_arena);
    if (!g_module) {
        host_error("Bytecode deserialize failed", 25);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    g_vm = vm_new(g_arena);
    if (!g_vm) {
        host_error("VM create failed", 16);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    vm_init(g_vm, g_module);
    vm_set_output(g_vm, wasm_output, NULL);
    vm_set_fetch(g_vm, wasm_fetch, NULL);
    vm_set_component_loader(g_vm, wasm_component_load, NULL);
    vm_set_linked_component_loader(g_vm, wasm_component_linked_load, NULL);
    vm_set_component_boundary_hooks(g_vm, wasm_component_start, wasm_component_end, NULL);
    vm_set_slot_default_hooks(g_vm, wasm_slot_default_start, wasm_slot_default_end, NULL);
    pending_clear();
    g_next_req_id = 1;
    g_state = STATE_IDLE;
    return MOT_OK;
}

WASM_EXPORT void mot_set_debug_component_markers(uint32_t enabled) {
    g_emit_component_markers = enabled != 0;
}

WASM_EXPORT void mot_set_debug_step_mode(uint32_t enabled) {
    g_emit_debug_steps = enabled != 0;
    if (!g_vm) return;
    if (g_emit_debug_steps) {
        vm_set_step_hook(g_vm, wasm_debug_step, NULL);
    } else {
        vm_set_step_hook(g_vm, NULL, NULL);
    }
}

WASM_EXPORT int mot_render(void) {
    VMResult result;
    if (!g_vm) {
        host_error("Runtime not initialized", 23);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    g_state = STATE_RUNNING;
    result = vm_run(g_vm);
    if (result == VM_AWAIT_DATA) {
        g_state = STATE_AWAITING;
        return MOT_AWAIT;
    }
    if (result != VM_OK) {
        const char *msg = vm_error_message(g_vm);
        if (msg && msg[0]) host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    g_state = STATE_DONE;
    host_render_complete();
    return MOT_OK;
}

WASM_EXPORT int mot_step(void) {
    VMResult result;
    if (!g_vm) {
        host_error("Runtime not initialized", 23);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    if (vm_execution_complete()) {
        g_state = STATE_DONE;
        return MOT_OK;
    }

    g_state = STATE_RUNNING;
    result = vm_step(g_vm);
    if (result == VM_AWAIT_DATA) {
        g_state = STATE_AWAITING;
        return MOT_AWAIT;
    }
    if (result != VM_OK) {
        const char *msg = vm_error_message(g_vm);
        if (msg && msg[0]) host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (vm_execution_complete()) {
        g_state = STATE_DONE;
        host_render_complete();
    }
    return MOT_OK;
}

WASM_EXPORT int mot_render_component(uint32_t func_idx,
                                     const char *props_json, uint32_t props_len,
                                     const char *children_json, uint32_t children_len) {
    Value props = val_null();
    Value children = val_null();
    const char *msg;

    if (!g_vm) {
        msg = "Runtime not initialized";
        host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (func_idx > 0xFFFFu) {
        msg = "Invalid component function index";
        host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if ((!props_json && props_len > 0) || (!children_json && children_len > 0)) {
        msg = "Component payload missing";
        host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (!parse_json_arg_or_null(props_json, props_len, &props, "Invalid component props JSON")) {
        msg = vm_error_message(g_vm);
        if (msg && msg[0]) host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (!parse_json_arg_or_null(children_json, children_len, &children, "Invalid component children JSON")) {
        msg = vm_error_message(g_vm);
        if (msg && msg[0]) host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (!prepare_component_render((uint16_t)func_idx, &props, &children)) {
        msg = vm_error_message(g_vm);
        if (msg && msg[0]) host_error(msg, (uint32_t)strlen(msg));
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    return mot_render();
}

WASM_EXPORT int mot_resume(uint32_t req_id, const char *json_data, uint32_t len) {
    if (!g_vm || g_pending.kind == PENDING_NONE) {
        host_error("No pending request", 18);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (req_id != g_pending.req_id) {
        host_error("Pending request id mismatch", 27);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (!json_data && len > 0) {
        host_error("Resume payload missing", 22);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (len >= sizeof(g_pending.payload)) {
        host_error("Resume payload too large", 24);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    if (len > 0 && json_data) {
        memcpy(g_pending.payload, json_data, len);
    }
    g_pending.payload[len] = '\0';
    g_pending.payload_len = len;
    g_pending.ready = true;
    vm_clear_await(g_vm);

    return mot_render();
}

WASM_EXPORT int mot_resume_step(uint32_t req_id, const char *json_data, uint32_t len) {
    if (!g_vm || g_pending.kind == PENDING_NONE) {
        host_error("No pending request", 18);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (req_id != g_pending.req_id) {
        host_error("Pending request id mismatch", 27);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (!json_data && len > 0) {
        host_error("Resume payload missing", 22);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }
    if (len >= sizeof(g_pending.payload)) {
        host_error("Resume payload too large", 24);
        g_state = STATE_ERROR;
        return MOT_ERROR;
    }

    if (len > 0 && json_data) {
        memcpy(g_pending.payload, json_data, len);
    }
    g_pending.payload[len] = '\0';
    g_pending.payload_len = len;
    g_pending.ready = true;
    vm_clear_await(g_vm);

    return mot_step();
}

WASM_EXPORT void mot_invalidate(const char *dep_path, uint32_t len) {
    (void)dep_path;
    (void)len;
}

WASM_EXPORT int mot_state(void) {
    return g_state;
}

WASM_EXPORT uint32_t mot_debug_snapshot_ptr(void) {
    return (uint32_t)(uintptr_t)g_debug_snapshot;
}

WASM_EXPORT uint32_t mot_debug_snapshot_len(void) {
    return build_debug_snapshot_json(g_debug_snapshot, (uint32_t)sizeof(g_debug_snapshot));
}

WASM_EXPORT void mot_free(void) {
    g_arena = NULL;
    g_module = NULL;
    g_vm = NULL;
    g_state = STATE_IDLE;
    pending_clear();
}

/* ================================================================
 * Compiler exports — compile .mot source to bytecode in-browser
 * ================================================================ */

static uint8_t *g_compiled_bc = NULL;
static uint32_t g_compiled_bc_len = 0;
static char    *g_compiled_css = NULL;
static uint32_t g_compiled_css_len = 0;
static char     g_compile_error[4096] = {0};
static uint32_t g_compile_error_len = 0;

WASM_EXPORT uint32_t mot_wasm_compile(uint32_t src_ptr, uint32_t src_len) {
    const char *source = (const char *)(uintptr_t)src_ptr;
    MotCompileResult result;

    g_compiled_bc = NULL;
    g_compiled_bc_len = 0;
    g_compiled_css = NULL;
    g_compiled_css_len = 0;
    g_compile_error[0] = '\0';
    g_compile_error_len = 0;

    result = mot_compile(source, (size_t)src_len);

    if (result.errors.count > 0) {
        /* Copy first error message */
        const char *msg = result.errors.errors[0].message;
        if (msg) {
            uint32_t len = (uint32_t)strlen(msg);
            if (len >= sizeof(g_compile_error)) len = sizeof(g_compile_error) - 1;
            memcpy(g_compile_error, msg, len);
            g_compile_error[len] = '\0';
            g_compile_error_len = len;
        }
        mot_result_free(&result);
        return 1; /* error */
    }

    g_compiled_bc = result.bytecode;
    g_compiled_bc_len = (uint32_t)result.bytecode_len;

    if (result.css) {
        g_compiled_css = result.css;
        g_compiled_css_len = (uint32_t)strlen(result.css);
    }

    /* Don't free result — we need the pointers alive.
       Memory is reclaimed on next mot_reset_alloc(). */
    return 0; /* ok */
}

WASM_EXPORT uint32_t mot_wasm_get_bytecode_ptr(void) {
    return (uint32_t)(uintptr_t)g_compiled_bc;
}

WASM_EXPORT uint32_t mot_wasm_get_bytecode_len(void) {
    return g_compiled_bc_len;
}

WASM_EXPORT uint32_t mot_wasm_get_error_ptr(void) {
    return (uint32_t)(uintptr_t)g_compile_error;
}

WASM_EXPORT uint32_t mot_wasm_get_error_len(void) {
    return g_compile_error_len;
}

WASM_EXPORT uint32_t mot_wasm_get_css_ptr(void) {
    return (uint32_t)(uintptr_t)g_compiled_css;
}

WASM_EXPORT uint32_t mot_wasm_get_css_len(void) {
    return g_compiled_css_len;
}
