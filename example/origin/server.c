/*
 * Motus Origin Server
 *
 * HTTP server that compiles .mot files on-demand and streams
 * bytecode to edge workers.
 *
 * Routes:
 *   GET /page/{path}        - Compile page and return bytecode
 *   GET /component/{name}   - Compile component and return bytecode
 *   POST /playground/compile - Compile arbitrary source and return bytecode
 *   POST /data/{page}       - Execute SQL by query reference and params
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sqlite3.h>

#ifdef __APPLE__
#include <sys/event.h>
#ifndef O_EVTONLY
#define O_EVTONLY 0x8000
#endif
#endif

/* Motus includes */
#include "../../src/mot.h"
#include "../../src/util/arena.h"
#include "../../src/parser/parser.h"
#include "../../src/compiler/bytecode.h"

#define PORT 8080
#define BUFFER_SIZE 4096
#define CONTENT_DIR "../content"
#define PRECOMPILE_PAGES_MANIFEST "../precompile-pages.txt"
#define PRECOMPILE_COMPONENTS_MANIFEST "../system-components.txt"
#define RUNTIME_WASM_PATH "../../runtime-wasm/build/mot-runtime.wasm"
#define DB_PATH "data.db"
#define MAX_DECODED_PATH 256
#define MAX_DECODED_QUERY 2048
#define MAX_REQUEST_SIZE 65536
#define MAX_REQUEST_BODY 16384
#define DEFAULT_CHUNK_SIZE 16384  /* 16 KB -- fits a typical TCP segment */

static volatile int running = 1;
static sqlite3 *db = NULL;
static size_t g_chunk_size = DEFAULT_CHUNK_SIZE;
static int g_dev_mode = 0;

/* ── Dev-mode SSE subscriber list ─────────────────────────────── */
#define MAX_SSE_CLIENTS 32
static int g_sse_clients[MAX_SSE_CLIENTS];
static int g_sse_count = 0;
static pthread_mutex_t g_sse_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct CompiledArtifact {
    char *key;                   /* logical route key: e.g. "index", "components/Header" */
    uint8_t *bytecode;           /* serialized bytecode */
    uint32_t bytecode_len;
    MotModule *mod;              /* opaque handle owning arena + module (kept for /data RPC) */
    struct CompiledArtifact *next;
} CompiledArtifact;

static CompiledArtifact *g_page_cache = NULL;
static CompiledArtifact *g_component_cache = NULL;
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static CompiledArtifact *get_or_compile_cached(CompiledArtifact **cache_head,
                                               const char *subdir,
                                               const char *key,
                                               bool keep_module,
                                               bool *out_not_found);

typedef struct LinkedComponentPath {
    char *path; /* normalized import path (e.g. components/Header) */
    struct LinkedComponentPath *next;
} LinkedComponentPath;

static LinkedComponentPath *g_linked_component_paths = NULL;

static size_t parse_chunk_size(const char *raw, size_t fallback) {
    char *end = NULL;
    unsigned long value;

    if (!raw || !*raw) return fallback;

    errno = 0;
    value = strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < 512 || value > 1048576) {
        return 0;  /* invalid */
    }

    return (size_t)value;
}

static int parse_port(const char *raw, int fallback) {
    char *end = NULL;
    long value;

    if (!raw || !*raw) return fallback;

    errno = 0;
    value = strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }

    return (int)value;
}

/* Signal handler for graceful shutdown */
static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* Initialize database with sample data */
static int init_database(void) {
    int rc = sqlite3_open(DB_PATH, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    /* Create tables and seed data */
    const char *sql =
        /* Products table */
        "CREATE TABLE IF NOT EXISTS products ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  price REAL NOT NULL,"
        "  description TEXT,"
        "  category TEXT,"
        "  in_stock INTEGER DEFAULT 1"
        ");"

        /* Categories table */
        "CREATE TABLE IF NOT EXISTS categories ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  description TEXT"
        ");"

        /* Posts table for blog-like content */
        "CREATE TABLE IF NOT EXISTS posts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL,"
        "  content TEXT,"
        "  author TEXT,"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        /* Check if products exist, if not seed data */
        "INSERT OR IGNORE INTO categories (id, name, description) VALUES "
        "(1, 'Electronics', 'Gadgets and devices'),"
        "(2, 'Audio', 'Headphones and speakers'),"
        "(3, 'Accessories', 'Cables, hubs, and more');"

        "INSERT OR IGNORE INTO products (id, name, price, description, category, in_stock) VALUES "
        "(1, 'Wireless Headphones', 99.99, 'Premium sound quality with active noise cancellation', 'Audio', 1),"
        "(2, 'Smart Watch', 199.99, 'Track your fitness and stay connected', 'Electronics', 1),"
        "(3, 'USB-C Hub', 49.99, '7-in-1 connectivity for your laptop', 'Accessories', 1),"
        "(4, 'Mechanical Keyboard', 149.99, 'Cherry MX switches with RGB lighting', 'Electronics', 1),"
        "(5, 'Bluetooth Speaker', 79.99, 'Portable speaker with 20hr battery', 'Audio', 1),"
        "(6, 'Webcam HD', 89.99, '1080p webcam with built-in mic', 'Electronics', 0),"
        "(7, 'Laptop Stand', 39.99, 'Ergonomic aluminum stand', 'Accessories', 1),"
        "(8, 'Wireless Mouse', 59.99, 'Precision tracking with long battery life', 'Accessories', 1);"

        "INSERT OR IGNORE INTO posts (id, title, content, author) VALUES "
        "(1, 'Welcome to Motus', 'This demo showcases streaming HTML from the edge using bytecode compilation.', 'The Motus Team'),"
        "(2, 'Edge Computing Benefits', 'Learn how edge computing reduces latency and improves user experience.', 'Tech Writer'),"
        "(3, 'SQL in Templates', 'Motus supports SQL queries directly in your templates for dynamic data.', 'Developer Guide');"

        /* Users table for authentication */
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  username TEXT UNIQUE NOT NULL,"
        "  password_hash TEXT NOT NULL,"
        "  role TEXT DEFAULT 'user',"
        "  created_at TEXT DEFAULT CURRENT_TIMESTAMP"
        ");"

        /* Sessions table */
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  user_id INTEGER NOT NULL,"
        "  token TEXT UNIQUE NOT NULL,"
        "  expires_at TEXT NOT NULL,"
        "  FOREIGN KEY (user_id) REFERENCES users(id)"
        ");"

        /* Seed demo admin user (djb2 hash) */
        "INSERT OR IGNORE INTO users (id, username, password_hash, role) VALUES "
        "(1, 'admin', '7c9e7c1b88e9a731', 'admin');";

    char *err_msg = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    printf("[db] Database initialized with sample data\n");
    return 0;
}

/* URL decode into a bounded buffer */
static bool url_decode(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src) return false;

    char a, b;
    size_t i = 0;
    while (*src) {
        if (i + 1 >= dst_size) {
            return false;
        }
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return true;
}

/* Validate a relative content path and prevent traversal */
static bool is_safe_relative_path(const char *path) {
    if (!path || !*path) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    if (strstr(path, "..")) return false;

    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '/' || c == '.')) {
            return false;
        }
    }
    return true;
}

/* Build safe content file path */
static bool build_content_filepath(char *dst, size_t dst_size,
                                   const char *subdir, const char *rel_path, bool append_ext) {
    if (!dst || dst_size == 0 || !subdir || !rel_path) return false;
    if (!is_safe_relative_path(rel_path)) return false;

    size_t rel_len = strlen(rel_path);
    bool has_ext = rel_len >= 4 && strcmp(rel_path + rel_len - 4, ".mot") == 0;
    const char *suffix = (append_ext && !has_ext) ? ".mot" : "";
    int written = snprintf(dst, dst_size, "%s/%s/%s%s", CONTENT_DIR, subdir, rel_path, suffix);
    return written > 0 && (size_t)written < dst_size;
}

static bool query_is_safe_select(const char *query) {
    if (!query) return false;

    size_t len = strlen(query);
    if (len == 0 || len > 1500) return false;

    /* Skip leading whitespace */
    while (*query && isspace((unsigned char)*query)) query++;
    if (strncasecmp(query, "select", 6) != 0) return false;

    /* Block obvious SQL injection/control sequences */
    if (strstr(query, ";") || strstr(query, "--") || strstr(query, "/*") || strstr(query, "*/")) {
        return false;
    }

    static const char *blocked[] = {
        "pragma", "attach", "detach", "drop", "delete",
        "insert", "update", "alter", "create", "replace", "vacuum"
    };

    char lower[MAX_DECODED_QUERY];
    size_t j = 0;
    for (; query[j] && j + 1 < sizeof(lower); j++) {
        lower[j] = (char)tolower((unsigned char)query[j]);
    }
    lower[j] = '\0';

    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        if (strstr(lower, blocked[i])) {
            return false;
        }
    }

    return true;
}

/* Portable case-insensitive substring search (ASCII) */
static const char *strcasestr_local(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (*needle == '\0') return haystack;

    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return h;
        }
    }
    return NULL;
}

/* Read file contents */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);

    if (out_len) *out_len = size;
    return content;
}

static char *dup_cstr(const char *s) {
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s);
    d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n + 1);
    return d;
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t slen, tlen;
    if (!s || !suffix) return false;
    slen = strlen(s);
    tlen = strlen(suffix);
    if (slen < tlen) return false;
    return strcmp(s + slen - tlen, suffix) == 0;
}

static void normalize_component_import_path(const char *input, char *out, size_t out_size) {
    size_t len = 0;
    const char *src = input ? input : "";
    if (!out || out_size == 0) return;

    while (*src && isspace((unsigned char)*src)) src++;
    while (src[0] == '.' && src[1] == '/') src += 2;
    len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) len--;
    if (len >= 4 && strncmp(src + len - 4, ".mot", 4) == 0) len -= 4;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

static bool linked_component_path_exists(const char *normalized_path) {
    for (LinkedComponentPath *it = g_linked_component_paths; it; it = it->next) {
        if (strcmp(it->path, normalized_path) == 0) return true;
    }
    return false;
}

static void register_linked_component_path(const char *raw_path) {
    char normalized[512];
    LinkedComponentPath *entry;
    if (!raw_path || !*raw_path) return;
    normalize_component_import_path(raw_path, normalized, sizeof(normalized));
    if (!normalized[0]) return;
    if (linked_component_path_exists(normalized)) return;

    entry = calloc(1, sizeof(LinkedComponentPath));
    if (!entry) return;
    entry->path = dup_cstr(normalized);
    if (!entry->path) {
        free(entry);
        return;
    }
    entry->next = g_linked_component_paths;
    g_linked_component_paths = entry;
}

static void load_linked_component_manifest(const char *manifest_path) {
    FILE *f;
    char line[1024];
    int count = 0;
    if (!manifest_path) return;
    f = fopen(manifest_path, "r");
    if (!f) {
        fprintf(stderr, "[link] Manifest not found: %s\n", manifest_path);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) p[--n] = '\0';
        if (n == 0) continue;
        register_linked_component_path(p);
        count++;
    }
    fclose(f);
    printf("[link] Loaded %d linked component path(s) from %s\n", count, manifest_path);
}

static bool compiler_resolve_linked_component(const char *import_path, void *userdata) {
    char normalized[512];
    (void)userdata;
    if (!import_path) return false;
    normalize_component_import_path(import_path, normalized, sizeof(normalized));
    if (!normalized[0]) return false;
    return linked_component_path_exists(normalized);
}

static void free_linked_component_paths(void) {
    LinkedComponentPath *it = g_linked_component_paths;
    while (it) {
        LinkedComponentPath *next = it->next;
        free(it->path);
        free(it);
        it = next;
    }
    g_linked_component_paths = NULL;
}

static BytecodeModule *resolve_linked_component_module_for_linker(const char *component_path, void *userdata) {
    char normalized[512];
    char key[768];
    bool not_found = false;
    CompiledArtifact *artifact;
    size_t subdir_len = strlen("components/");
    (void)userdata;

    if (!component_path || !*component_path) return NULL;
    normalize_component_import_path(component_path, normalized, sizeof(normalized));
    if (!normalized[0]) return NULL;

    if (strncmp(normalized, "components/", subdir_len) == 0) {
        snprintf(key, sizeof(key), "%s", normalized + subdir_len);
    } else {
        snprintf(key, sizeof(key), "%s", normalized);
    }
    if (!key[0]) return NULL;
    if (has_suffix(key, ".mot")) {
        key[strlen(key) - 4] = '\0';
    }

    artifact = get_or_compile_cached(&g_component_cache, "components", key, true, &not_found);
    if (!artifact || !artifact->mod) return NULL;
    return mot_module_bytecode(artifact->mod);
}

static CompiledArtifact *cache_find_unlocked(CompiledArtifact *head, const char *key) {
    for (CompiledArtifact *it = head; it; it = it->next) {
        if (strcmp(it->key, key) == 0) {
            return it;
        }
    }
    return NULL;
}

/* Entries are append-only for process lifetime (no hot-reload replacement), so
 * returning pointers after unlock is safe in this demo server. */
static CompiledArtifact *cache_lookup(CompiledArtifact *head, const char *key) {
    CompiledArtifact *found;
    pthread_mutex_lock(&g_cache_mutex);
    found = cache_find_unlocked(head, key);
    pthread_mutex_unlock(&g_cache_mutex);
    return found;
}

static CompiledArtifact *cache_insert_if_absent(CompiledArtifact **head, CompiledArtifact *entry) {
    CompiledArtifact *existing;
    if (!head || !entry) return NULL;

    pthread_mutex_lock(&g_cache_mutex);
    existing = cache_find_unlocked(*head, entry->key);
    if (!existing) {
        entry->next = *head;
        *head = entry;
        existing = entry;
    }
    pthread_mutex_unlock(&g_cache_mutex);
    return existing;
}

static void free_compiled_artifact(CompiledArtifact *entry) {
    if (!entry) return;
    free(entry->key);
    free(entry->bytecode);
    mot_module_free(entry->mod);
    free(entry);
}

static void free_cache_list(CompiledArtifact *head) {
    while (head) {
        CompiledArtifact *next = head->next;
        free_compiled_artifact(head);
        head = next;
    }
}

/* ── Dev-mode SSE helpers ──────────────────────────────────────── */

static void sse_add_client(int fd) {
    pthread_mutex_lock(&g_sse_mutex);
    if (g_sse_count < MAX_SSE_CLIENTS) {
        g_sse_clients[g_sse_count++] = fd;
    } else {
        close(fd);
    }
    pthread_mutex_unlock(&g_sse_mutex);
}

static void sse_broadcast(const char *event_type, const char *path) {
    char msg[1024];
    int len = snprintf(msg, sizeof(msg),
        "event: %s\ndata: {\"path\":\"%s\"}\n\n",
        event_type, path);

    pthread_mutex_lock(&g_sse_mutex);
    int i = 0;
    while (i < g_sse_count) {
        ssize_t n = send(g_sse_clients[i], msg, len, 0);
        if (n <= 0) {
            close(g_sse_clients[i]);
            g_sse_clients[i] = g_sse_clients[--g_sse_count];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_sse_mutex);
}

static void sse_close_all(void) {
    pthread_mutex_lock(&g_sse_mutex);
    for (int i = 0; i < g_sse_count; i++) {
        close(g_sse_clients[i]);
    }
    g_sse_count = 0;
    pthread_mutex_unlock(&g_sse_mutex);
}

static void handle_dev_events(int client) {
    char headers[512];
    int len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n"
        "\r\n");
    send(client, headers, len, 0);

    /* Send initial heartbeat */
    const char *hello = "event: connected\ndata: {}\n\n";
    send(client, hello, strlen(hello), 0);

    /* Hand ownership of the fd to the SSE subscriber list.
     * The client_thread must NOT close(client) afterwards. */
    sse_add_client(client);
}

/* ── Dev-mode file watcher (kqueue on macOS) ──────────────────── */

#ifdef __APPLE__

#define WATCH_MAX_FDS 512

typedef struct {
    int    fds[WATCH_MAX_FDS];
    char  *paths[WATCH_MAX_FDS];   /* strdup'd path for each fd */
    int    count;
} WatchSet;

static void watch_set_clear(WatchSet *ws) {
    for (int i = 0; i < ws->count; i++) {
        close(ws->fds[i]);
        free(ws->paths[i]);
    }
    ws->count = 0;
}

/* Register one path with kqueue. Returns true on success. */
static bool watch_set_add(WatchSet *ws, int kq, const char *path) {
    if (ws->count >= WATCH_MAX_FDS) return false;
    int fd = open(path, O_RDONLY | O_EVTONLY);
    if (fd < 0) return false;

    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_VNODE,
           EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB | NOTE_EXTEND,
           0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        close(fd);
        return false;
    }
    ws->fds[ws->count] = fd;
    ws->paths[ws->count] = strdup(path);
    ws->count++;
    return true;
}

/* Recursively register all directories AND .mot files under dir_path. */
static void watch_tree(WatchSet *ws, int kq, const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    /* Watch the directory itself (catches new-file creation / renames) */
    watch_set_add(ws, kq, dir_path);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            watch_tree(ws, kq, path);
        } else if (has_suffix(ent->d_name, ".mot")) {
            watch_set_add(ws, kq, path);
        }
    }
    closedir(d);
}

static void *dev_watcher_thread(void *arg) {
    (void)arg;
    int kq = kqueue();
    if (kq < 0) {
        perror("[dev] kqueue");
        return NULL;
    }

    WatchSet ws = { .count = 0 };
    watch_tree(&ws, kq, CONTENT_DIR);

    printf("[dev] Watching %s for changes (%d files/dirs)\n",
           CONTENT_DIR, ws.count);

    while (running) {
        struct kevent events[16];
        struct timespec timeout = { .tv_sec = 2, .tv_nsec = 0 };
        int n = kevent(kq, NULL, 0, events, 16, &timeout);
        if (n <= 0) {
            /* Periodic rescan: editors that do atomic saves (write-tmp + rename)
             * replace the inode, so the old fd becomes stale.  Re-register
             * the whole tree every timeout cycle to pick up new inodes. */
            watch_set_clear(&ws);
            watch_tree(&ws, kq, CONTENT_DIR);
            continue;
        }

        /* Debounce: coalesce rapid events into one broadcast */
        usleep(100000); /* 100ms */

        /* Identify which file(s) changed by mapping fd → path */
        char changed_path[512] = "";
        for (int e = 0; e < n; e++) {
            int efd = (int)events[e].ident;
            for (int w = 0; w < ws.count; w++) {
                if (ws.fds[w] == efd && ws.paths[w]) {
                    /* Strip CONTENT_DIR prefix to get relative path like
                     * "components/ProductCard.mot" or "pages/about.mot" */
                    const char *rel = ws.paths[w];
                    size_t prefix_len = strlen(CONTENT_DIR);
                    if (strncmp(rel, CONTENT_DIR, prefix_len) == 0) {
                        rel += prefix_len;
                        if (*rel == '/') rel++;
                    }
                    /* Strip .mot extension */
                    size_t rlen = strlen(rel);
                    if (rlen < sizeof(changed_path)) {
                        memcpy(changed_path, rel, rlen + 1);
                        if (rlen >= 4 && strcmp(changed_path + rlen - 4, ".mot") == 0) {
                            changed_path[rlen - 4] = '\0';
                        }
                    }
                    break;
                }
            }
            if (changed_path[0]) break; /* use first matched file */
        }

        /* Drain any additional events that arrived during debounce */
        struct timespec zero = { 0, 0 };
        while (kevent(kq, NULL, 0, events, 16, &zero) > 0) { /* drain */ }

        if (!changed_path[0]) {
            snprintf(changed_path, sizeof(changed_path), "content");
        }
        printf("[dev] Changed: %s, broadcasting reload\n", changed_path);
        sse_broadcast("change", changed_path);

        /* Re-register watches (handles atomic saves that replaced inodes) */
        watch_set_clear(&ws);
        watch_tree(&ws, kq, CONTENT_DIR);
    }

    watch_set_clear(&ws);
    close(kq);
    return NULL;
}
#endif /* __APPLE__ */

/* Send HTTP response headers */
static void send_headers(int client, int status, const char *content_type, size_t content_length) {
    char headers[512];
    const char *status_text = status == 200 ? "OK" :
                              status == 404 ? "Not Found" :
                              status == 500 ? "Internal Server Error" : "Unknown";

    int len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, content_length);

    send(client, headers, len, 0);
}

/* Send chunked response start */
static void send_chunked_start(int client, const char *content_type) {
    char headers[512];
    int len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type);

    send(client, headers, len, 0);
}

/* Send a chunk */
static void send_chunk(int client, const void *data, size_t len) {
    char size_line[20];
    int size_len = snprintf(size_line, sizeof(size_line), "%zx\r\n", len);
    send(client, size_line, size_len, 0);
    if (len > 0) {
        send(client, data, len, 0);
    }
    send(client, "\r\n", 2, 0);
}

/* Send chunked response end */
static void send_chunked_end(int client) {
    send_chunk(client, NULL, 0);
}

/* Send error response */
static void send_error(int client, int status, const char *message) {
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"error\": \"%s\"}", message);
    send_headers(client, status, "application/json", body_len);
    send(client, body, body_len, 0);
}

/* Escape a string for JSON */
static void json_escape(char *dst, const char *src, size_t max_len) {
    size_t i = 0;
    while (*src && i < max_len - 2) {
        switch (*src) {
            case '"':  if (i + 2 < max_len) { dst[i++] = '\\'; dst[i++] = '"'; } break;
            case '\\': if (i + 2 < max_len) { dst[i++] = '\\'; dst[i++] = '\\'; } break;
            case '\n': if (i + 2 < max_len) { dst[i++] = '\\'; dst[i++] = 'n'; } break;
            case '\r': if (i + 2 < max_len) { dst[i++] = '\\'; dst[i++] = 'r'; } break;
            case '\t': if (i + 2 < max_len) { dst[i++] = '\\'; dst[i++] = 't'; } break;
            default:   dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
}

typedef enum {
    JSON_VALUE_NULL,
    JSON_VALUE_BOOL,
    JSON_VALUE_NUMBER,
    JSON_VALUE_STRING,
    JSON_VALUE_OTHER
} JsonValueType;

static const char *skip_ws_range(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static bool json_find_u32(const char *json, const char *key, uint32_t *out) {
    if (!json || !key || !out) return false;

    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) return false;

    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += plen;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (!isdigit((unsigned char)*pos)) return false;

    char *endptr = NULL;
    unsigned long value = strtoul(pos, &endptr, 10);
    if (endptr == pos || value > 0xFFFFFFFFUL) return false;
    *out = (uint32_t)value;
    return true;
}

static bool json_find_object_bounds(const char *json, const char *key,
                                    const char **obj_start, const char **obj_end) {
    if (!json || !key || !obj_start || !obj_end) return false;

    char pattern[64];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (plen <= 0 || (size_t)plen >= sizeof(pattern)) return false;

    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += plen;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != '{') return false;

    const char *start = pos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; *pos; pos++) {
        char ch = *pos;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') depth++;
        if (ch == '}') {
            depth--;
            if (depth == 0) {
                *obj_start = start;
                *obj_end = pos + 1;
                return true;
            }
        }
    }

    return false;
}

static bool json_scan_value(const char *p, const char *end,
                            const char **value_end, JsonValueType *type) {
    if (!p || p >= end || !value_end || !type) return false;

    if (*p == '"') {
        p++;
        bool escaped = false;
        for (; p < end; p++) {
            if (escaped) {
                escaped = false;
            } else if (*p == '\\') {
                escaped = true;
            } else if (*p == '"') {
                *value_end = p + 1;
                *type = JSON_VALUE_STRING;
                return true;
            }
        }
        return false;
    }

    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (; p < end; p++) {
            char ch = *p;
            if (in_string) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') in_string = false;
                continue;
            }
            if (ch == '"') {
                in_string = true;
                continue;
            }
            if (ch == open) depth++;
            if (ch == close) {
                depth--;
                if (depth == 0) {
                    *value_end = p + 1;
                    *type = JSON_VALUE_OTHER;
                    return true;
                }
            }
        }
        return false;
    }

    const char *start = p;
    while (p < end && *p != ',' && *p != '}') p++;
    const char *trimmed_end = p;
    while (trimmed_end > start && isspace((unsigned char)trimmed_end[-1])) trimmed_end--;
    if (trimmed_end <= start) return false;

    size_t len = (size_t)(trimmed_end - start);
    if ((len == 4 && strncmp(start, "null", 4) == 0)) *type = JSON_VALUE_NULL;
    else if ((len == 4 && strncmp(start, "true", 4) == 0) ||
             (len == 5 && strncmp(start, "false", 5) == 0)) *type = JSON_VALUE_BOOL;
    else *type = JSON_VALUE_NUMBER;

    *value_end = trimmed_end;
    return true;
}

static bool json_find_object_value(const char *obj_start, const char *obj_end,
                                   const char *key,
                                   const char **value_start, const char **value_end,
                                   JsonValueType *type) {
    if (!obj_start || !obj_end || !key || !value_start || !value_end || !type) return false;

    const char *p = skip_ws_range(obj_start, obj_end);
    if (p >= obj_end || *p != '{') return false;
    p++;

    while (p < obj_end) {
        p = skip_ws_range(p, obj_end);
        if (p >= obj_end) break;
        if (*p == '}') return false;
        if (*p != '"') return false;

        p++;
        const char *key_start = p;
        bool escaped = false;
        while (p < obj_end) {
            if (escaped) {
                escaped = false;
            } else if (*p == '\\') {
                escaped = true;
            } else if (*p == '"') {
                break;
            }
            p++;
        }
        if (p >= obj_end || *p != '"') return false;
        const char *key_end = p;
        p++;

        p = skip_ws_range(p, obj_end);
        if (p >= obj_end || *p != ':') return false;
        p++;
        p = skip_ws_range(p, obj_end);
        if (p >= obj_end) return false;

        const char *v_start = p;
        const char *v_end = NULL;
        JsonValueType v_type = JSON_VALUE_OTHER;
        if (!json_scan_value(v_start, obj_end, &v_end, &v_type)) return false;

        size_t key_len = (size_t)(key_end - key_start);
        if (strlen(key) == key_len && memcmp(key_start, key, key_len) == 0) {
            *value_start = v_start;
            *value_end = v_end;
            *type = v_type;
            return true;
        }

        p = skip_ws_range(v_end, obj_end);
        if (p < obj_end && *p == ',') {
            p++;
            continue;
        }
        if (p < obj_end && *p == '}') {
            return false;
        }
    }

    return false;
}

static bool json_unescape_to_buffer(const char *start, const char *end, char *dst, size_t dst_size) {
    if (!start || !end || !dst || dst_size == 0) return false;
    if (start >= end || *start != '"' || end[-1] != '"') return false;

    size_t i = 0;
    const char *p = start + 1;
    const char *limit = end - 1;
    while (p < limit) {
        if (i + 1 >= dst_size) return false;
        char ch = *p++;
        if (ch == '\\' && p < limit) {
            char esc = *p++;
            switch (esc) {
                case '"': ch = '"'; break;
                case '\\': ch = '\\'; break;
                case '/': ch = '/'; break;
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                default: return false;
            }
        }
        dst[i++] = ch;
    }
    dst[i] = '\0';
    return true;
}

/* ========== Authentication Helpers ========== */

/* Simple djb2 hash for passwords (demo only - use bcrypt/argon2 in production) */
static void simple_hash(const char *input, char *output, size_t out_size) {
    unsigned long hash = 5381;
    int c;
    while ((c = *input++)) {
        hash = ((hash << 5) + hash) + c;
    }
    snprintf(output, out_size, "%016lx", hash);
}

/* Generate secure random token (64 hex chars) */
static void generate_token(char *out, size_t out_size) {
    uint8_t bytes[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, bytes, sizeof(bytes));
        close(fd);
    } else {
        /* Fallback to time-based random (less secure) */
        srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
        for (size_t i = 0; i < sizeof(bytes); i++) {
            bytes[i] = (uint8_t)(rand() & 0xFF);
        }
    }
    for (size_t i = 0; i < sizeof(bytes) && i * 2 + 2 < out_size; i++) {
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    }
}

/* Parse a cookie value from request headers */
static bool parse_cookie(const char *request, const char *name, char *out, size_t out_size) {
    if (!request || !name || !out || out_size == 0) return false;
    out[0] = '\0';

    const char *cookie_header = strcasestr_local(request, "\r\nCookie:");
    if (!cookie_header) return false;
    cookie_header += 9; /* Skip "\r\nCookie:" */

    const char *line_end = strstr(cookie_header, "\r\n");
    if (!line_end) line_end = cookie_header + strlen(cookie_header);

    /* Search for cookie name */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    size_t pattern_len = strlen(pattern);

    const char *pos = cookie_header;
    while (pos < line_end) {
        /* Skip whitespace */
        while (pos < line_end && (*pos == ' ' || *pos == ';')) pos++;
        if (pos >= line_end) break;

        if (strncmp(pos, pattern, pattern_len) == 0) {
            pos += pattern_len;
            size_t i = 0;
            while (pos < line_end && *pos != ';' && *pos != '\r' && i < out_size - 1) {
                out[i++] = *pos++;
            }
            out[i] = '\0';
            return i > 0;
        }
        /* Skip to next cookie */
        while (pos < line_end && *pos != ';') pos++;
    }
    return false;
}

/* Extract JSON string value */
static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    if (!json || !key || !out || out_size == 0) return false;

    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return false;
    pos += strlen(pattern);

    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos && isspace((unsigned char)*pos)) pos++;
    if (*pos != '"') return false;
    pos++; /* Skip opening quote */

    size_t i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        if (*pos == '\\' && pos[1]) {
            pos++;
            switch (*pos) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *pos; break;
            }
        } else {
            out[i++] = *pos;
        }
        pos++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool json_extract_string_alloc(const char *json, size_t json_len, const char *key, char **out) {
    const char *value_start = NULL;
    const char *value_end = NULL;
    JsonValueType value_type = JSON_VALUE_OTHER;
    size_t raw_len = 0;
    char *decoded = NULL;
    if (!json || !key || !out) return false;
    *out = NULL;

    if (!json_find_object_value(json, json + json_len, key, &value_start, &value_end, &value_type)) {
        return false;
    }
    if (value_type != JSON_VALUE_STRING) {
        return false;
    }
    if (!value_start || !value_end || value_end <= value_start + 1) {
        return false;
    }

    raw_len = (size_t)(value_end - value_start - 2);
    decoded = malloc(raw_len + 1);
    if (!decoded) return false;
    if (!json_unescape_to_buffer(value_start, value_end, decoded, raw_len + 1)) {
        free(decoded);
        return false;
    }

    *out = decoded;
    return true;
}

/* Handle POST /login */
static void handle_login(int client, const char *body, size_t body_len) {
    if (!body || body_len == 0) {
        send_error(client, 400, "Missing request body");
        return;
    }

    char username[64], password[64];
    if (!json_extract_string(body, "username", username, sizeof(username)) ||
        !json_extract_string(body, "password", password, sizeof(password))) {
        send_error(client, 400, "Missing username or password");
        return;
    }

    /* Hash password and compare */
    char password_hash[32];
    simple_hash(password, password_hash, sizeof(password_hash));

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, role FROM users WHERE username = ? AND password_hash = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        send_error(client, 500, "Database error");
        return;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password_hash, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        send_error(client, 401, "Invalid credentials");
        return;
    }

    int user_id = sqlite3_column_int(stmt, 0);
    const char *role = (const char *)sqlite3_column_text(stmt, 1);
    char role_copy[32];
    snprintf(role_copy, sizeof(role_copy), "%s", role ? role : "user");
    sqlite3_finalize(stmt);

    /* Generate session token */
    char token[65];
    generate_token(token, sizeof(token));

    /* Calculate expiry (24 hours from now) */
    time_t expires = time(NULL) + 86400;
    char expires_str[32];
    struct tm *tm = gmtime(&expires);
    strftime(expires_str, sizeof(expires_str), "%Y-%m-%d %H:%M:%S", tm);

    /* Insert session */
    const char *insert_sql = "INSERT INTO sessions (user_id, token, expires_at) VALUES (?, ?, ?)";
    sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, token, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, expires_str, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    printf("[auth] User '%s' logged in (session: %.8s...)\n", username, token);

    /* Send response with Set-Cookie header */
    char response[256];
    int resp_len = snprintf(response, sizeof(response),
        "{\"success\":true,\"user\":{\"id\":%d,\"username\":\"%s\",\"role\":\"%s\"}}",
        user_id, username, role_copy);

    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Set-Cookie: session=%s; HttpOnly; Path=/; Max-Age=86400\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Credentials: true\r\n"
        "Connection: close\r\n"
        "\r\n", resp_len, token);

    send(client, headers, hdr_len, 0);
    send(client, response, resp_len, 0);
}

/* Handle POST /logout */
static void handle_logout(int client, const char *request) {
    char token[65] = {0};
    parse_cookie(request, "session", token, sizeof(token));

    if (token[0]) {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token = ?", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, token, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        printf("[auth] Session logged out: %.8s...\n", token);
    }

    const char *body = "{\"success\":true}";
    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Set-Cookie: session=; HttpOnly; Path=/; Max-Age=0\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Credentials: true\r\n"
        "Connection: close\r\n"
        "\r\n", strlen(body));

    send(client, headers, hdr_len, 0);
    send(client, body, strlen(body), 0);
}

/* Handle GET /session - validate session and return user info */
static void handle_session(int client, const char *request) {
    char token[65] = {0};
    parse_cookie(request, "session", token, sizeof(token));

    if (!token[0]) {
        send_error(client, 401, "No session");
        return;
    }

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT u.id, u.username, u.role FROM users u "
        "JOIN sessions s ON s.user_id = u.id "
        "WHERE s.token = ? AND datetime(s.expires_at) > datetime('now')";

    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        send_error(client, 401, "Invalid or expired session");
        return;
    }

    int user_id = sqlite3_column_int(stmt, 0);
    const char *username = (const char *)sqlite3_column_text(stmt, 1);
    const char *role = (const char *)sqlite3_column_text(stmt, 2);

    char response[256];
    int resp_len = snprintf(response, sizeof(response),
        "{\"valid\":true,\"user\":{\"id\":%d,\"username\":\"%s\",\"role\":\"%s\"}}",
        user_id, username ? username : "", role ? role : "user");

    sqlite3_finalize(stmt);

    send_headers(client, 200, "application/json", resp_len);
    send(client, response, resp_len, 0);
}

/* ========== End Authentication Helpers ========== */

static bool bind_sql_params(sqlite3_stmt *stmt, const DataRequirement *req, const char *json_body) {
    if (!stmt || !req) return false;
    if (req->param_count == 0) return true;
    if (!json_body) return false;

    const char *params_start = NULL;
    const char *params_end = NULL;
    if (!json_find_object_bounds(json_body, "params", &params_start, &params_end)) {
        return false;
    }

    for (uint16_t i = 0; i < req->param_count; i++) {
        const char *param_name = req->param_names[i];
        const char *value_start = NULL;
        const char *value_end = NULL;
        JsonValueType value_type = JSON_VALUE_OTHER;

        if (!json_find_object_value(params_start, params_end, param_name,
                                    &value_start, &value_end, &value_type)) {
            return false;
        }

        char sqlite_name[96];
        int n = snprintf(sqlite_name, sizeof(sqlite_name), ":%s", param_name);
        if (n <= 1 || (size_t)n >= sizeof(sqlite_name)) return false;
        int bind_idx = sqlite3_bind_parameter_index(stmt, sqlite_name);
        if (bind_idx == 0) return false;

        int rc = SQLITE_ERROR;
        if (value_type == JSON_VALUE_NULL) {
            rc = sqlite3_bind_null(stmt, bind_idx);
        } else if (value_type == JSON_VALUE_BOOL) {
            bool b = (value_end - value_start == 4 && strncmp(value_start, "true", 4) == 0);
            rc = sqlite3_bind_int(stmt, bind_idx, b ? 1 : 0);
        } else if (value_type == JSON_VALUE_NUMBER) {
            size_t len = (size_t)(value_end - value_start);
            if (len == 0 || len >= 64) return false;
            char num[64];
            memcpy(num, value_start, len);
            num[len] = '\0';

            bool is_float = false;
            for (size_t k = 0; k < len; k++) {
                if (num[k] == '.' || num[k] == 'e' || num[k] == 'E') {
                    is_float = true;
                    break;
                }
            }
            if (is_float) {
                rc = sqlite3_bind_double(stmt, bind_idx, strtod(num, NULL));
            } else {
                rc = sqlite3_bind_int64(stmt, bind_idx, strtoll(num, NULL, 10));
            }
        } else if (value_type == JSON_VALUE_STRING) {
            size_t raw_len = (size_t)(value_end - value_start);
            if (raw_len < 2 || raw_len > 4096) return false;
            char *tmp = malloc(raw_len + 1);
            if (!tmp) return false;
            bool ok = json_unescape_to_buffer(value_start, value_end, tmp, raw_len + 1);
            if (!ok) {
                free(tmp);
                return false;
            }
            rc = sqlite3_bind_text(stmt, bind_idx, tmp, -1, SQLITE_TRANSIENT);
            free(tmp);
        } else {
            return false;
        }

        if (rc != SQLITE_OK) return false;
    }

    return true;
}

/* Execute SQL and return JSON */
static char *execute_sql(const DataRequirement *req, const char *json_body, size_t *out_len) {
    if (!req || !query_is_safe_select(req->query)) {
        return NULL;
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, req->query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[sql] Prepare error: %s\n", sqlite3_errmsg(db));
        return NULL;
    }

    if (!bind_sql_params(stmt, req, json_body)) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    /* Build JSON array */
    size_t buf_size = 16384;
    char *json = malloc(buf_size);
    if (!json) {
        sqlite3_finalize(stmt);
        return NULL;
    }

    size_t pos = 0;
    json[pos++] = '[';

    int col_count = sqlite3_column_count(stmt);
    int row_num = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (row_num > 0) {
            json[pos++] = ',';
        }
        json[pos++] = '{';

        for (int i = 0; i < col_count; i++) {
            if (i > 0) json[pos++] = ',';

            const char *col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);

            /* Write key */
            pos += snprintf(json + pos, buf_size - pos, "\"%s\":", col_name);

            /* Write value based on type */
            switch (col_type) {
                case SQLITE_INTEGER:
                    pos += snprintf(json + pos, buf_size - pos, "%lld", sqlite3_column_int64(stmt, i));
                    break;
                case SQLITE_FLOAT:
                    pos += snprintf(json + pos, buf_size - pos, "%.2f", sqlite3_column_double(stmt, i));
                    break;
                case SQLITE_TEXT: {
                    const char *text = (const char *)sqlite3_column_text(stmt, i);
                    char escaped[1024];
                    json_escape(escaped, text ? text : "", sizeof(escaped));
                    pos += snprintf(json + pos, buf_size - pos, "\"%s\"", escaped);
                    break;
                }
                case SQLITE_NULL:
                    pos += snprintf(json + pos, buf_size - pos, "null");
                    break;
                default:
                    pos += snprintf(json + pos, buf_size - pos, "null");
                    break;
            }

            /* Grow buffer if needed */
            if (pos > buf_size - 1024) {
                buf_size *= 2;
                json = realloc(json, buf_size);
            }
        }

        json[pos++] = '}';
        row_num++;
    }

    json[pos++] = ']';
    json[pos] = '\0';

    sqlite3_finalize(stmt);

    if (out_len) *out_len = pos;
    return json;
}

/* Track already imported paths to avoid duplicates */
typedef struct ImportedPath {
    char *path;
    struct ImportedPath *next;
} ImportedPath;

static bool is_already_imported(ImportedPath *list, const char *path) {
    for (ImportedPath *p = list; p; p = p->next) {
        if (strcmp(p->path, path) == 0) return true;
    }
    return false;
}

static void tag_top_level_defcomp_sources(AstNode *ast, const char *source_path) {
    if (!ast || !source_path) return;
    AstNode *children = (ast->type == NODE_DOCUMENT) ? ast->data.document.children : ast;
    for (AstNode *node = children; node; node = node->next) {
        if (node->type == NODE_DEFCOMP) {
            node->source_path = source_path;
        }
    }
}

/* Resolve static imports by loading and parsing component files
 * Returns a list of defcomp nodes to prepend to the AST */
static AstNode *resolve_imports(Arena *arena, AstNode *ast, ImportedPath **imported) {
    AstNode *defcomps = NULL;
    AstNode **tail = &defcomps;

    /* Get children list - ast is a document node */
    AstNode *children = (ast->type == NODE_DOCUMENT) ? ast->data.document.children : ast;

    /* Walk through children looking for imports */
    for (AstNode *node = children; node; node = node->next) {
        if (node->type != NODE_IMPORT) continue;
        if (node->data.import.is_dynamic) continue;  /* Skip dynamic imports */

        const char *from_path = node->data.import.from_path;

        /* Skip if already imported */
        if (is_already_imported(*imported, from_path)) continue;

        /* Mark as imported */
        ImportedPath *imp = arena_alloc(arena, sizeof(ImportedPath));
        imp->path = arena_strdup(arena, from_path);
        imp->next = *imported;
        *imported = imp;

        /* Build file path */
        char filepath[512];
        if (!build_content_filepath(filepath, sizeof(filepath), "", from_path, true)) {
            fprintf(stderr, "[import] Rejected unsafe import path: %s\n", from_path);
            continue;
        }

        /* Load and parse component file */
        size_t comp_len;
        char *comp_source = read_file(filepath, &comp_len);
        if (!comp_source) {
            fprintf(stderr, "[import] Failed to load: %s\n", filepath);
            continue;
        }

        printf("[import] Loading: %s\n", from_path);

        Parser parser;
        parser_init(&parser, comp_source, comp_len, arena, NULL);
        AstNode *comp_ast = parser_parse(&parser);
        free(comp_source);

        if (!comp_ast || parser.had_error) {
            fprintf(stderr, "[import] Parse error in: %s\n", from_path);
            continue;
        }
        /* Recursively resolve imports in component (modifies comp_ast in place) */
        resolve_imports(arena, comp_ast, imported);

        /* Extract dynamic imports + defcomp definitions from component in source
         * order so dynamically imported dependencies used by imported defcomps
         * are still registered during compile. */
        AstNode *comp_children = (comp_ast->type == NODE_DOCUMENT) ? comp_ast->data.document.children : comp_ast;
        for (AstNode *cn = comp_children; cn; ) {
            AstNode *next = cn->next;  /* Save before we modify */
            if (cn->type == NODE_IMPORT && cn->data.import.is_dynamic) {
                cn->next = NULL;
                *tail = cn;
                tail = &cn->next;
            } else if (cn->type == NODE_DEFCOMP) {
                printf("[import] Found defcomp: %s\n", cn->data.defcomp.name);
                /* Preserve nested component provenance resolved in recursive import
                 * calls; only assign source_path when this defcomp does not already
                 * carry one. */
                if (!cn->source_path || !cn->source_path[0]) {
                    cn->source_path = from_path;
                }
                cn->next = NULL;  /* Terminate this defcomp */
                *tail = cn;
                tail = &cn->next;
            }
            cn = next;
        }
    }

    /* Terminate the defcomps list */
    *tail = NULL;

    /* Prepend defcomps to document's children and remove import nodes */
    if (ast->type == NODE_DOCUMENT) {
        /* Remove static import nodes from children */
        AstNode **ptr = &ast->data.document.children;
        while (*ptr) {
            if ((*ptr)->type == NODE_IMPORT && !(*ptr)->data.import.is_dynamic) {
                /* Skip this import node */
                *ptr = (*ptr)->next;
            } else {
                ptr = &(*ptr)->next;
            }
        }

        /* Prepend defcomps */
        if (defcomps) {
            int count = 0;
            for (AstNode *d = defcomps; d; d = d->next) count++;
            printf("[import] Prepending %d defcomp(s) to document\n", count);

            /* Find last defcomp */
            AstNode *last = defcomps;
            while (last->next) last = last->next;
            /* Link to existing children */
            last->next = ast->data.document.children;
            /* Update document's children pointer */
            ast->data.document.children = defcomps;
        }
    }

    return ast;
}

static MotModule *compile_mot_module(const char *source, size_t source_len,
                                     const char *source_path) {
    Arena *arena = arena_create(128 * 1024);  /* Larger arena for imports */
    if (!arena) return NULL;

    /* Parse */
    Parser parser;
    parser_init(&parser, source, source_len, arena, NULL);
    AstNode *ast = parser_parse(&parser);

    if (!ast || parser.had_error) {
        fprintf(stderr, "[compile] Parse error\n");
        arena_destroy(arena);
        return NULL;
    }
    tag_top_level_defcomp_sources(ast, source_path);

    /* Resolve static imports */
    ImportedPath *imported = NULL;
    ast = resolve_imports(arena, ast, &imported);
    printf("[compile] Import resolution complete\n");

    /* Analyze + compile via library API (transfers arena ownership on success) */
    MotCompileOptions opts;
    opts.target = MOT_TARGET_BYTECODE;
    opts.partial_eval = true;
    opts.include_debug = false;
    opts.linked_component_resolver = compiler_resolve_linked_component;
    opts.linked_component_userdata = NULL;

    MotErrorList errors;
    memset(&errors, 0, sizeof(errors));

    MotModule *mod = mot_compile_ast(arena, ast, &opts, &errors);
    if (!mod) {
        for (size_t i = 0; i < errors.count; i++) {
            fprintf(stderr, "[compile] %s\n", errors.errors[i].message);
        }
        if (errors.count == 0)
            fprintf(stderr, "[compile] Compilation failed\n");
        mot_error_list_free(&errors);
        arena_destroy(arena);  /* caller still owns arena on failure */
        return NULL;
    }
    mot_error_list_free(&errors);
    printf("[compile] Compilation complete\n");

    /* Link resolved components into the module */
    {
        char link_error[256] = {0};
        if (!mot_link_resolve_module(
                mot_module_bytecode(mod),
                resolve_linked_component_module_for_linker,
                NULL,
                link_error,
                sizeof(link_error))) {
            fprintf(stderr, "[compile] Linked component resolution failed: %s\n",
                    link_error[0] ? link_error : "unknown linker error");
            mot_module_free(mod);
            return NULL;
        }
    }

    return mod;
}

static CompiledArtifact *compile_artifact_from_source(const char *key,
                                                       const char *source_path,
                                                       const char *source,
                                                       size_t source_len,
                                                       bool keep_module) {
    MotModule *mod = compile_mot_module(source, source_len, source_path);
    if (!mod) return NULL;

    uint32_t bytecode_len = 0;
    uint8_t *serialized = mot_module_serialize(mod, &bytecode_len, false);
    if (!serialized || bytecode_len == 0) {
        mot_module_free(mod);
        return NULL;
    }

    CompiledArtifact *artifact = calloc(1, sizeof(CompiledArtifact));
    if (!artifact) {
        free(serialized);
        mot_module_free(mod);
        return NULL;
    }

    artifact->key = dup_cstr(key);
    artifact->bytecode = serialized;
    artifact->bytecode_len = bytecode_len;
    if (keep_module) {
        artifact->mod = mod;
    } else {
        artifact->mod = NULL;
        mot_module_free(mod);
    }
    return artifact;
}

static CompiledArtifact *compile_artifact_from_file(const char *subdir, const char *key,
                                                    bool keep_module, bool *out_not_found) {
    char filepath[512];
    char source_path[768];
    size_t source_len = 0;
    char *source;

    if (out_not_found) *out_not_found = false;
    if (!build_content_filepath(filepath, sizeof(filepath), subdir, key, true)) {
        return NULL;
    }

    source = read_file(filepath, &source_len);
    if (!source) {
        if (out_not_found) *out_not_found = true;
        return NULL;
    }

    if (subdir && subdir[0] != '\0') {
        snprintf(source_path, sizeof(source_path), "%s/%s.mot", subdir, key);
    } else {
        snprintf(source_path, sizeof(source_path), "%s.mot", key);
    }
    CompiledArtifact *artifact = compile_artifact_from_source(
        key, source_path, source, source_len, keep_module
    );
    free(source);
    return artifact;
}

static CompiledArtifact *get_or_compile_cached(CompiledArtifact **cache_head,
                                               const char *subdir,
                                               const char *key,
                                               bool keep_module,
                                               bool *out_not_found) {
    /* In dev mode, always recompile from disk (late binding). */
    if (g_dev_mode) {
        bool not_found = false;
        CompiledArtifact *compiled = compile_artifact_from_file(subdir, key, keep_module, &not_found);
        if (!compiled) {
            if (out_not_found) *out_not_found = not_found;
            return NULL;
        }
        if (out_not_found) *out_not_found = false;
        /* NOTE: caller in handle_page/handle_component currently expects the artifact
         * to live for the duration of the response.  In dev mode we leak small artifacts
         * rather than threading a free-after-send path; acceptable for a local dev tool. */
        return compiled;
    }

    CompiledArtifact *cached = cache_lookup(*cache_head, key);
    if (cached) {
        if (out_not_found) *out_not_found = false;
        return cached;
    }

    bool not_found = false;
    CompiledArtifact *compiled = compile_artifact_from_file(subdir, key, keep_module, &not_found);
    if (!compiled) {
        if (out_not_found) *out_not_found = not_found;
        return NULL;
    }

    CompiledArtifact *winner = cache_insert_if_absent(cache_head, compiled);
    if (winner != compiled) {
        free_compiled_artifact(compiled);
    }

    if (out_not_found) *out_not_found = false;
    return winner;
}

static void precompile_from_manifest(CompiledArtifact **cache_head,
                                     const char *subdir,
                                     const char *manifest_path,
                                     bool keep_module,
                                     int *ok_count,
                                     int *fail_count) {
    FILE *f = fopen(manifest_path, "r");
    if (!f) {
        fprintf(stderr, "[precompile] Manifest not found: %s\n", manifest_path);
        return;
    }

    char line[1024];
    size_t subdir_len = strlen(subdir);
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        size_t n = strlen(p);
        while (n > 0 && isspace((unsigned char)p[n - 1])) {
            p[--n] = '\0';
        }
        if (n == 0) continue;

        char key[768];
        if (n >= sizeof(key)) {
            (*fail_count)++;
            continue;
        }
        memcpy(key, p, n + 1);

        if (strncmp(key, subdir, subdir_len) == 0 && key[subdir_len] == '/') {
            memmove(key, key + subdir_len + 1, strlen(key + subdir_len + 1) + 1);
        }
        if (has_suffix(key, ".mot")) {
            key[strlen(key) - 4] = '\0';
        }
        if (!is_safe_relative_path(key)) {
            fprintf(stderr, "[precompile] Invalid key in %s: %s\n", manifest_path, key);
            (*fail_count)++;
            continue;
        }

        if (cache_lookup(*cache_head, key)) {
            continue;
        }

        printf("[precompile] compiling %s/%s\n", subdir, key);
        bool not_found = false;
        CompiledArtifact *compiled = compile_artifact_from_file(subdir, key, keep_module, &not_found);
        if (!compiled) {
            fprintf(stderr, "[precompile] Failed: %s/%s\n", subdir, key);
            (*fail_count)++;
            continue;
        }

        CompiledArtifact *winner = cache_insert_if_absent(cache_head, compiled);
        if (winner != compiled) {
            free_compiled_artifact(compiled);
        }
        printf("[precompile] cached %s/%s (%u bytes)\n", subdir, key, winner->bytecode_len);
        (*ok_count)++;
    }

    fclose(f);
}

static void precompile_startup_assets(void) {
    int pages_ok = 0, pages_fail = 0;
    int comps_ok = 0, comps_fail = 0;

    precompile_from_manifest(&g_page_cache, "pages", PRECOMPILE_PAGES_MANIFEST, true, &pages_ok, &pages_fail);
    precompile_from_manifest(&g_component_cache, "components", PRECOMPILE_COMPONENTS_MANIFEST,
                             true, &comps_ok, &comps_fail);

    printf("[precompile] pages: %d ok, %d failed | components: %d ok, %d failed\n",
           pages_ok, pages_fail, comps_ok, comps_fail);
}

/* Handle /page/{path} - compile page */
static void handle_page(int client, const char *path) {
    if (!is_safe_relative_path(path)) {
        send_error(client, 400, "Invalid page path");
        return;
    }

    bool not_found = false;
    CompiledArtifact *artifact = get_or_compile_cached(&g_page_cache, "pages", path, true, &not_found);
    if (!artifact) {
        send_error(client, not_found ? 404 : 500, not_found ? "Page not found" : "Compilation failed");
        return;
    }

    printf("[origin] Serving page bytecode: %s (%u bytes)\n", path, artifact->bytecode_len);
    send_chunked_start(client, "application/octet-stream");

    size_t offset = 0;
    while (offset < artifact->bytecode_len) {
        size_t chunk = artifact->bytecode_len - offset;
        if (chunk > g_chunk_size) chunk = g_chunk_size;
        send_chunk(client, artifact->bytecode + offset, chunk);
        offset += chunk;
    }

    send_chunked_end(client);
}

/* Handle /component/{name} - compile component */
static void handle_component(int client, const char *name) {
    if (!is_safe_relative_path(name)) {
        send_error(client, 400, "Invalid component path");
        return;
    }

    bool not_found = false;
    CompiledArtifact *artifact = get_or_compile_cached(&g_component_cache, "components", name, true, &not_found);
    if (!artifact) {
        send_error(client, not_found ? 404 : 500, not_found ? "Component not found" : "Compilation failed");
        return;
    }

    send_headers(client, 200, "application/octet-stream", artifact->bytecode_len);
    send(client, artifact->bytecode, artifact->bytecode_len, 0);
    printf("[origin] Serving component bytecode: %s (%u bytes)\n", name, artifact->bytecode_len);
}

/* Handle /source/{path} - serve raw .mot source for debug tooling */
static void handle_source(int client, const char *source_path) {
    if (!is_safe_relative_path(source_path)) {
        send_error(client, 400, "Invalid source path");
        return;
    }
    if (!has_suffix(source_path, ".mot")) {
        send_error(client, 400, "Source path must end with .mot");
        return;
    }

    const char *subdir = NULL;
    const char *rel_path = NULL;
    if (strncmp(source_path, "pages/", 6) == 0) {
        subdir = "pages";
        rel_path = source_path + 6;
    } else if (strncmp(source_path, "components/", 11) == 0) {
        subdir = "components";
        rel_path = source_path + 11;
    } else {
        send_error(client, 400, "Source path must start with pages/ or components/");
        return;
    }

    if (!rel_path || !*rel_path || !is_safe_relative_path(rel_path)) {
        send_error(client, 400, "Invalid source path");
        return;
    }

    char filepath[1024];
    if (!build_content_filepath(filepath, sizeof(filepath), subdir, rel_path, false)) {
        send_error(client, 400, "Invalid source path");
        return;
    }

    size_t source_len = 0;
    char *source = read_file(filepath, &source_len);
    if (!source) {
        send_error(client, 404, "Source not found");
        return;
    }

    send_headers(client, 200, "text/plain; charset=utf-8", source_len);
    send(client, source, source_len, 0);
    printf("[origin] Serving source: %s (%zu bytes)\n", source_path, source_len);
    free(source);
}

/* Handle /runtime/wasm - serve prebuilt browser runtime wasm module */
static void handle_runtime_wasm(int client) {
    size_t wasm_len = 0;
    char *wasm = read_file(RUNTIME_WASM_PATH, &wasm_len);
    if (!wasm || wasm_len == 0) {
        free(wasm);
        send_error(client, 500, "Runtime wasm not available");
        return;
    }

    send_headers(client, 200, "application/wasm", wasm_len);
    send(client, wasm, wasm_len, 0);
    printf("[origin] Serving runtime wasm: %s (%zu bytes)\n", RUNTIME_WASM_PATH, wasm_len);
    free(wasm);
}

/* Handle POST /data/{page} with JSON body:
 * {"queryRef":N,"signature":N,"params":{...},"single":bool}
 */
static void handle_data_rpc(int client, const char *page_path, const char *body, size_t body_len) {
    if (!body || body_len == 0 || body_len > MAX_REQUEST_BODY) {
        send_error(client, 400, "Invalid request body");
        return;
    }

    uint32_t query_ref = 0;
    uint32_t signature = 0;
    if (!json_find_u32(body, "queryRef", &query_ref) ||
        !json_find_u32(body, "signature", &signature)) {
        send_error(client, 400, "Missing queryRef/signature");
        return;
    }

    if (!is_safe_relative_path(page_path)) {
        send_error(client, 400, "Invalid page path");
        return;
    }

    bool not_found = false;
    CompiledArtifact *artifact = get_or_compile_cached(&g_page_cache, "pages", page_path, true, &not_found);
    if (!artifact || !artifact->mod) {
        send_error(client, not_found ? 404 : 500,
                   not_found ? "Page not found" : "Failed to load precompiled page");
        return;
    }
    BytecodeModule *module = mot_module_bytecode(artifact->mod);

    if (query_ref >= module->data_req_count) {
        send_error(client, 400, "Invalid query reference");
        return;
    }

    DataRequirement *req = &module->data_reqs[query_ref];
    if (req->signature != signature || req->query_ref != query_ref) {
        send_error(client, 403, "Query signature mismatch");
        return;
    }

    printf("[data] page=%s ref=%u params=%u\n", page_path, query_ref, req->param_count);

    size_t json_len = 0;
    char *json = execute_sql(req, body, &json_len);
    if (!json) {
        send_error(client, 400, "Query execution failed");
        return;
    }

    send_headers(client, 200, "application/json", json_len);
    send(client, json, json_len, 0);
    free(json);
}

/* Handle POST /playground/compile with JSON body:
 * {"source":"<mot source>"}
 */
static void handle_playground_compile(int client, const char *body, size_t body_len) {
    char *source = NULL;

    if (!body || body_len == 0 || body_len > MAX_REQUEST_BODY) {
        send_error(client, 400, "Invalid request body");
        return;
    }

    if (!json_extract_string_alloc(body, body_len, "source", &source) || !source || !source[0]) {
        free(source);
        send_error(client, 400, "Missing source");
        return;
    }

    MotModule *mod = compile_mot_module(source, strlen(source), "playground/input.mot");
    if (!mod) {
        free(source);
        send_error(client, 400, "Compilation failed");
        return;
    }

    uint32_t bytecode_len = 0;
    uint8_t *bytecode = mot_module_serialize(mod, &bytecode_len, false);
    if (!bytecode || bytecode_len == 0) {
        free(source);
        mot_module_free(mod);
        send_error(client, 500, "Bytecode serialization failed");
        return;
    }

    send_headers(client, 200, "application/octet-stream", bytecode_len);
    send(client, bytecode, bytecode_len, 0);

    free(bytecode);
    free(source);
    mot_module_free(mod);
}

static bool parse_content_length(const char *request, size_t header_len, size_t *out_len) {
    if (!request || !out_len) return false;
    *out_len = 0;

    const char *p = request;
    const char *end = request + header_len;
    while (p < end) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end || line_end > end) break;

        size_t line_len = (size_t)(line_end - p);
        if (line_len >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *num = p + 15;
            while (num < line_end && isspace((unsigned char)*num)) num++;
            unsigned long v = strtoul(num, NULL, 10);
            if (v > MAX_REQUEST_BODY) return false;
            *out_len = (size_t)v;
            return true;
        }
        p = line_end + 2;
    }

    return true;
}

/* Parse HTTP request and route.
 * Returns true if the handler took ownership of the fd (SSE). */
static bool handle_request(int client) {
    bool owns_fd = false;
    char *buffer = malloc(MAX_REQUEST_SIZE + 1);
    if (!buffer) {
        send_error(client, 500, "Server memory error");
        return false;
    }

    size_t total = 0;
    size_t header_len = 0;
    size_t content_len = 0;

    while (total < MAX_REQUEST_SIZE) {
        ssize_t n = recv(client, buffer + total, MAX_REQUEST_SIZE - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buffer[total] = '\0';

        if (header_len == 0) {
            char *hdr_end = strstr(buffer, "\r\n\r\n");
            if (hdr_end) {
                header_len = (size_t)(hdr_end - buffer) + 4;
                if (!parse_content_length(buffer, header_len, &content_len)) {
                    free(buffer);
                    send_error(client, 400, "Invalid Content-Length");
                    return false;
                }
            }
        }

        if (header_len > 0 && total >= header_len + content_len) {
            break;
        }
    }

    if (total == 0 || header_len == 0) {
        free(buffer);
        return false;
    }

    /* Parse method and path */
    char method[16], path[256];
    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        free(buffer);
        send_error(client, 400, "Bad request");
        return false;
    }

    /* Decode URL */
    char decoded_path[MAX_DECODED_PATH];
    if (!url_decode(decoded_path, sizeof(decoded_path), path)) {
        free(buffer);
        send_error(client, 400, "Path too long");
        return false;
    }

    printf("[origin] %s %s\n", method, decoded_path);

    const char *body = (header_len <= total) ? (buffer + header_len) : NULL;
    size_t body_len = (header_len <= total) ? (total - header_len) : 0;

    /* Route request */
    if (g_dev_mode && strcmp(method, "GET") == 0 && strcmp(decoded_path, "/_dev/events") == 0) {
        handle_dev_events(client);
        owns_fd = true;  /* SSE handler takes ownership */
    } else if (strcmp(method, "GET") == 0 && strncmp(decoded_path, "/page/", 6) == 0) {
        handle_page(client, decoded_path + 6);
    } else if (strcmp(method, "GET") == 0 && strncmp(decoded_path, "/component/", 11) == 0) {
        handle_component(client, decoded_path + 11);
    } else if (strcmp(method, "GET") == 0 && strncmp(decoded_path, "/source/", 8) == 0) {
        handle_source(client, decoded_path + 8);
    } else if (strcmp(method, "POST") == 0 && strcmp(decoded_path, "/playground/compile") == 0) {
        handle_playground_compile(client, body, body_len);
    } else if (strcmp(method, "GET") == 0 && strcmp(decoded_path, "/runtime/wasm") == 0) {
        handle_runtime_wasm(client);
    } else if (strcmp(method, "POST") == 0 && strncmp(decoded_path, "/data/", 6) == 0) {
        handle_data_rpc(client, decoded_path + 6, body, body_len);
    } else if (strcmp(method, "POST") == 0 && strcmp(decoded_path, "/login") == 0) {
        handle_login(client, body, body_len);
    } else if (strcmp(method, "POST") == 0 && strcmp(decoded_path, "/logout") == 0) {
        handle_logout(client, buffer);
    } else if (strcmp(method, "GET") == 0 && strcmp(decoded_path, "/session") == 0) {
        handle_session(client, buffer);
    } else if (strcmp(method, "GET") == 0 && strcmp(decoded_path, "/health") == 0) {
        const char *ok = g_dev_mode
            ? "{\"status\":\"ok\",\"dev\":true}"
            : "{\"status\":\"ok\"}";
        send_headers(client, 200, "application/json", strlen(ok));
        send(client, ok, strlen(ok), 0);
    } else if (strcmp(method, "GET") != 0 && strcmp(method, "POST") != 0) {
        send_error(client, 405, "Method not allowed");
    } else {
        send_error(client, 404, "Not found");
    }

    free(buffer);
    return owns_fd;
}

/* Client handler thread */
static void *client_thread(void *arg) {
    int client = *(int *)arg;
    free(arg);

    bool owns_fd = handle_request(client);
    if (!owns_fd) {
        close(client);
    }

    return NULL;
}

int main(int argc, char **argv) {
    int port = PORT;
    const char *env_port = getenv("ORIGIN_PORT");
    const char *env_chunk = getenv("MOT_CHUNK_SIZE");
    const char *env_dev = getenv("MOT_DEV");

    if (env_dev && (strcmp(env_dev, "1") == 0 || strcasecmp(env_dev, "true") == 0)) {
        g_dev_mode = 1;
    }

    int env_parsed = parse_port(env_port, port);
    if (env_parsed == -1) {
        fprintf(stderr, "Invalid ORIGIN_PORT: %s\n", env_port);
        return 1;
    }
    port = env_parsed;

    if (env_chunk) {
        size_t cs = parse_chunk_size(env_chunk, 0);
        if (cs == 0) {
            fprintf(stderr, "Invalid MOT_CHUNK_SIZE: %s (must be 512..1048576)\n", env_chunk);
            return 1;
        }
        g_chunk_size = cs;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0) {
            g_dev_mode = 1;
        } else if (strcmp(argv[i], "--no-dev") == 0) {
            g_dev_mode = 0;
        } else if (strcmp(argv[i], "--chunk-size") == 0 && i + 1 < argc) {
            size_t cs = parse_chunk_size(argv[++i], 0);
            if (cs == 0) {
                fprintf(stderr, "Invalid --chunk-size: %s (must be 512..1048576)\n", argv[i]);
                return 1;
            }
            g_chunk_size = cs;
        } else {
            int arg_parsed = parse_port(argv[i], -1);
            if (arg_parsed == -1) {
                fprintf(stderr, "Invalid port argument: %s\n", argv[i]);
                return 1;
            }
            port = arg_parsed;
        }
    }

    /* Initialize database */
    if (init_database() != 0) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }

    load_linked_component_manifest(PRECOMPILE_COMPONENTS_MANIFEST);

    if (g_dev_mode) {
        printf("[dev] Dev mode enabled — late binding, no precompilation\n");
    } else {
        /* Precompile startup pages/components into memory caches (production). */
        precompile_startup_assets();
    }

    /* Set up signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Create socket */
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        sqlite3_close(db);
        return 1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server);
        sqlite3_close(db);
        return 1;
    }

    /* Listen */
    if (listen(server, 10) < 0) {
        perror("listen");
        close(server);
        sqlite3_close(db);
        return 1;
    }

    printf("=================================\n");
    printf("Motus Origin Server%s\n", g_dev_mode ? " (DEV)" : "");
    printf("Listening on http://localhost:%d\n", port);
    printf("=================================\n");
    printf("\nRoutes:\n");
    printf("  GET /page/{name}      - Compile and stream page bytecode\n");
    printf("  GET /component/{name} - Compile and return component\n");
    printf("  GET /source/{path}    - Serve raw .mot source for debug\n");
    printf("  POST /playground/compile - Compile source and return bytecode\n");
    printf("  GET /runtime/wasm     - Serve browser runtime wasm module\n");
    printf("  POST /data/{page}     - Execute SQL by reference and params\n");
    printf("  POST /login           - Authenticate user\n");
    printf("  POST /logout          - End session\n");
    printf("  GET /session          - Validate session\n");
    printf("  GET /health           - Health check\n");
    if (g_dev_mode) {
        printf("  GET /_dev/events      - SSE stream for hot-reload\n");
    }
    printf("\nDatabase: %s\n", DB_PATH);
    printf("Chunk size: %zu bytes\n", g_chunk_size);
    if (g_dev_mode) {
        printf("Mode: DEV (late binding, no cache, file watcher active)\n");
    }
    printf("Press Ctrl+C to stop\n\n");

#ifdef __APPLE__
    /* Start file watcher in dev mode */
    if (g_dev_mode) {
        pthread_t watcher;
        if (pthread_create(&watcher, NULL, dev_watcher_thread, NULL) == 0) {
            pthread_detach(watcher);
        } else {
            fprintf(stderr, "[dev] Warning: failed to start file watcher\n");
        }
    }
#endif

    /* Accept loop */
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(server, (struct sockaddr *)&client_addr, &client_len);

        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        /* Handle in thread */
        int *client_ptr = malloc(sizeof(int));
        *client_ptr = client;

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, client_ptr) != 0) {
            close(client);
            free(client_ptr);
        } else {
            pthread_detach(thread);
        }
    }

    close(server);
    sse_close_all();
    free_cache_list(g_page_cache);
    free_cache_list(g_component_cache);
    free_linked_component_paths();
    sqlite3_close(db);
    printf("\nServer stopped\n");
    return 0;
}
