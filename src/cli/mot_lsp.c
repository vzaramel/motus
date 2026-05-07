/*
 * Motus Language Server Protocol Server
 * 
 * A stdio-based LSP server that provides:
 * - Diagnostics (errors/warnings from parsing)
 * - Code completion
 * - Hover information
 * - Go to definition
 * - Document symbols
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "analyzer/analyzer.h"
#include "compiler/compiler.h"
#include "util/arena.h"
#include "util/str.h"

/* Maximums */
#define MAX_MESSAGE_SIZE (1024 * 1024)
#define MAX_LINE_LENGTH 4096
#define MAX_DOCUMENTS 64
#define MAX_SYMBOLS 256
#define MAX_DIAGNOSTICS 64
#define MAX_IMPORTS 32

/* Forward declarations */
typedef struct LSPDocument LSPDocument;
typedef struct LSPServer LSPServer;

/* LSP Document */
struct LSPDocument {
    char *uri;
    char *file_path;  /* File path for the URI */
    char *content;
    size_t content_len;
    int version;
    
    /* Parsed symbols */
    struct {
        char *name;
        int kind;
        int line;
        int col;
        int end_line;
        int end_col;
    } symbols[MAX_SYMBOLS];
    int symbol_count;
    
    /* Import statements */
    struct {
        char *local_name;    /* e.g., "Layout" */
        char *import_path;  /* e.g., "components/Layout" */
        int line;
        int col;
        int end_col;
    } imports[MAX_IMPORTS];
    int import_count;
    
    /* Diagnostics */
    struct {
        int line;
        int col;
        int end_line;
        int end_col;
        int severity;
        char *message;
    } diagnostics[MAX_DIAGNOSTICS];
    int diag_count;
    
    LSPDocument *next;
};

/* LSP Server */
struct LSPServer {
    LSPDocument *documents;
    int doc_count;
    bool initialized;
    
    /* Capabilities */
    bool has_completion;
    bool has_hover;
    bool has_definition;
    bool has_document_symbol;
};

/* ============================================================================
 * String Utilities
 * ============================================================================ */

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *r = malloc(len + 1);
    if (r) memcpy(r, s, len + 1);
    return r;
}

static char *str_ndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > n) len = n;
    char *r = malloc(len + 1);
    if (r) {
        memcpy(r, s, len);
        r[len] = '\0';
    }
    return r;
}

static char *str_rstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return NULL;
    for (const char *p = haystack + hlen - nlen; p >= haystack; p--) {
        if (strncmp(p, needle, nlen) == 0) {
            return (char *)p;
        }
    }
    return NULL;
}

/* ============================================================================
 * JSON Parsing (Minimal)
 * ============================================================================ */

typedef enum {
    JSON_NULL,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_OBJECT,
    JSON_ARRAY
} JsonType;

/* ============================================================================
 * Document Management
 * ============================================================================ */

static LSPDocument *find_document(LSPServer *server, const char *uri) {
    for (LSPDocument *doc = server->documents; doc; doc = doc->next) {
        if (strcmp(doc->uri, uri) == 0) return doc;
    }
    return NULL;
}

static void add_document(LSPServer *server, const char *uri, const char *content) {
    if (server->doc_count >= MAX_DOCUMENTS) return;
    
    LSPDocument *doc = calloc(1, sizeof(LSPDocument));
    if (!doc) return;
    
    doc->uri = str_dup(uri);
    doc->content = str_dup(content);
    doc->content_len = strlen(content);
    doc->version = 1;
    
    /* Extract file path from URI (file:///path/to/file.mot) */
    if (strncmp(uri, "file://", 7) == 0) {
        doc->file_path = str_dup(uri + 7);
    }
    
    /* Add to list */
    doc->next = server->documents;
    server->documents = doc;
    server->doc_count++;
}

static void update_document(LSPServer *server, const char *uri, const char *content) {
    LSPDocument *doc = find_document(server, uri);
    if (doc) {
        free(doc->content);
        doc->content = str_dup(content);
        doc->content_len = strlen(content);
        doc->version++;
    }
}

static void remove_document(LSPServer *server, const char *uri) {
    LSPDocument *prev = NULL;
    for (LSPDocument *doc = server->documents; doc; doc = doc->next) {
        if (strcmp(doc->uri, uri) == 0) {
            if (prev) prev->next = doc->next;
            else server->documents = doc->next;
            free(doc->uri);
            free(doc->content);
            free(doc);
            server->doc_count--;
            return;
        }
        prev = doc;
    }
}

/* ============================================================================
 * Symbol Parsing
 * ============================================================================ */

static void parse_document_symbols(LSPDocument *doc) {
    doc->symbol_count = 0;
    doc->import_count = 0;
    if (!doc->content) {
        fprintf(stderr, "[LSP] parse_document_symbols: no content!\n");
        return;
    }
    
    const char *content = doc->content;
    int line = 0;
    const char *line_start = content;
    
    while (line_start < content + doc->content_len && doc->symbol_count < MAX_SYMBOLS) {
        const char *line_end = strchr(line_start, '\n');
        if (!line_end) line_end = content + doc->content_len;
        
        fprintf(stderr, "[LSP] Parsing line %d: %.40s...\n", line, line_start);
        
        /* Find defcomp */
        const char *defcomp = strstr(line_start, "<defcomp");
        if (defcomp && defcomp < line_end) {
            const char *name = defcomp + 8;
            while (name < line_end && (*name == ' ' || *name == '|')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            if (name_end > name) {
                doc->symbols[doc->symbol_count].name = str_ndup(name, name_end - name);
                doc->symbols[doc->symbol_count].kind = 7; /* Class */
                doc->symbols[doc->symbol_count].line = line;
                doc->symbols[doc->symbol_count].col = (int)(name - line_start);
                doc->symbols[doc->symbol_count].end_line = line;
                doc->symbols[doc->symbol_count].end_col = (int)(name_end - line_start);
                doc->symbol_count++;
            }
        }
        
        /* Find macro */
        const char *macro = strstr(line_start, "<macro");
        if (macro && macro < line_end) {
            const char *name = macro + 6;
            while (name < line_end && (*name == ' ' || *name == '|')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            if (name_end > name) {
                doc->symbols[doc->symbol_count].name = str_ndup(name, name_end - name);
                doc->symbols[doc->symbol_count].kind = 14; /* Function */
                doc->symbols[doc->symbol_count].line = line;
                doc->symbols[doc->symbol_count].col = (int)(name - line_start);
                doc->symbols[doc->symbol_count].end_line = line;
                doc->symbols[doc->symbol_count].end_col = (int)(name_end - line_start);
                doc->symbol_count++;
            }
        }
        
        /* Find interface */
        const char *iface = strstr(line_start, "<interface");
        if (iface && iface < line_end) {
            const char *name = iface + 9;
            while (name < line_end && (*name == ' ' || *name == '|')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            if (name_end > name) {
                doc->symbols[doc->symbol_count].name = str_ndup(name, name_end - name);
                doc->symbols[doc->symbol_count].kind = 8; /* Interface */
                doc->symbols[doc->symbol_count].line = line;
                doc->symbols[doc->symbol_count].col = (int)(name - line_start);
                doc->symbols[doc->symbol_count].end_line = line;
                doc->symbols[doc->symbol_count].end_col = (int)(name_end - line_start);
                doc->symbol_count++;
            }
        }
        
        /* Find let */
        const char *let = strstr(line_start, "<let ");
        if (let && let < line_end) {
            const char *name = let + 5;
            while (name < line_end && (*name == ' ')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            if (name_end > name) {
                doc->symbols[doc->symbol_count].name = str_ndup(name, name_end - name);
                doc->symbols[doc->symbol_count].kind = 6; /* Variable */
                doc->symbols[doc->symbol_count].line = line;
                doc->symbols[doc->symbol_count].col = (int)(name - line_start);
                doc->symbols[doc->symbol_count].end_line = line;
                doc->symbols[doc->symbol_count].end_col = (int)(name_end - line_start);
                doc->symbol_count++;
            }
        }
        
        /* Find var */
        const char *var = strstr(line_start, "<var ");
        if (var && var < line_end && var != let) {
            const char *name = var + 5;
            while (name < line_end && (*name == ' ')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            if (name_end > name) {
                doc->symbols[doc->symbol_count].name = str_ndup(name, name_end - name);
                doc->symbols[doc->symbol_count].kind = 6; /* Variable */
                doc->symbols[doc->symbol_count].line = line;
                doc->symbols[doc->symbol_count].col = (int)(name - line_start);
                doc->symbols[doc->symbol_count].end_line = line;
                doc->symbols[doc->symbol_count].end_col = (int)(name_end - line_start);
                doc->symbol_count++;
            }
        }
        
        /* Find imports: <import Name from "path"> */
        const char *imp = strstr(line_start, "<import ");
        if (imp && imp < line_end && doc->import_count < MAX_IMPORTS) {
            const char *name = imp + 8;
            while (name < line_end && (*name == ' ')) name++;
            const char *name_end = name;
            while (name_end < line_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
            
            /* Find the path */
            const char *from = strstr(line_start, "from \"");
            if (from && from < line_end) {
                const char *path_start = from + 6;
                const char *path_end = strchr(path_start, '"');
                if (path_end && path_end < line_end) {
                    doc->imports[doc->import_count].local_name = str_ndup(name, name_end - name);
                    doc->imports[doc->import_count].import_path = str_ndup(path_start, path_end - path_start);
                    doc->imports[doc->import_count].line = line;
                    doc->imports[doc->import_count].col = (int)(name - line_start);
                    doc->imports[doc->import_count].end_col = (int)(name_end - line_start);
                    doc->import_count++;
                }
            }
        }
        
        line_start = line_end + 1;
        fprintf(stderr, "[LSP] Next line_start=%p, content+len=%p, diff=%zu\n", 
                line_start, content + doc->content_len, 
                (size_t)(content + doc->content_len - line_start));
        line++;
    }
}

/* ============================================================================
 * Diagnostics
 * ============================================================================ */

static void parse_diagnostics(LSPDocument *doc) {
    doc->diag_count = 0;
    if (!doc->content) return;
    
    Arena *arena = arena_create(8192);
    if (!arena) return;
    
    /* Try to parse the document */
    Parser parser;
    parser_init(&parser, doc->content, doc->content_len, arena, NULL);
    AstNode *ast = parser_parse(&parser);
    
    if (parser_had_error(&parser) && parser.errors) {
        /* Add parser errors as diagnostics */
        for (size_t i = 0; i < parser.errors->count && doc->diag_count < MAX_DIAGNOSTICS; i++) {
            doc->diagnostics[doc->diag_count].line = parser.errors->errors[i].line - 1;
            doc->diagnostics[doc->diag_count].col = parser.errors->errors[i].column;
            doc->diagnostics[doc->diag_count].end_line = doc->diagnostics[doc->diag_count].line;
            doc->diagnostics[doc->diag_count].end_col = doc->diagnostics[doc->diag_count].col + 10;
            doc->diagnostics[doc->diag_count].severity = 1; /* Error */
            doc->diagnostics[doc->diag_count].message = str_dup(parser.errors->errors[i].message);
            doc->diag_count++;
        }
    }
    
    (void)ast;  /* Suppress unused warning */
    arena_destroy(arena);
}

/* ============================================================================
 * JSON Response Building
 * ============================================================================ */

static void send_response(const char *json) {
    size_t len = strlen(json);
    printf("Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(stdout);
}

static void send_notification(const char *method, const char *params_json) {
    fprintf(stderr, "[LSP] Sending notification: %s\n", method);
    char json[8192];
    snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
        method, params_json ? params_json : "{}");
    send_response(json);
}

static void send_error_response(int64_t id, int code, const char *message) {
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\",\"id\":%" PRId64 ",\"error\":{\"code\":%d,\"message\":\"%s\"}}",
        id, code, message);
    send_response(json);
}

static void send_response_with_result(int64_t id, const char *result_json) {
    char json[8192];
    snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\",\"id\":%" PRId64 ",\"params\":%s}",
        id, result_json);
    send_response(json);
}

/* ============================================================================
 * LSP Method Handlers
 * ============================================================================ */

static void handle_initialize(LSPServer *server, int64_t id, const char *params) {
    (void)params;  /* Not used currently */
    server->initialized = true;
    server->has_completion = true;
    server->has_hover = true;
    server->has_definition = true;
    server->has_document_symbol = true;
    
    const char *result =
        "{"
        "\"capabilities\":{"
        "\"textDocumentSync\":2,"
        "\"completionProvider\":{\"resolveProvider\":false,\"triggerCharacters\":[\"<\",\" \",\":\"]},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"documentSymbolProvider\":true"
        "},"
        "\"serverInfo\":{"
        "\"name\":\"Motus Language Server\","
        "\"version\":\"0.1.0\""
        "}"
        "}";
    
    char json[8192];
    snprintf(json, sizeof(json),
        "{\"jsonrpc\":\"2.0\",\"id\":%" PRId64 ",\"result\":%s}", id, result);
    send_response(json);
}

static void handle_initialized(LSPServer *server, const char *params) {
    (void)server;
    (void)params;
    /* Nothing to do */
}

static void handle_shutdown(LSPServer *server, int64_t id, const char *params) {
    (void)server;
    (void)params;
    send_response_with_result(id, "null");
}

static void handle_exit(LSPServer *server, const char *params) {
    (void)server;
    (void)params;
    exit(0);
}

static void handle_textDocument_didOpen(LSPServer *server, const char *params) {
    /* Parse: {"textDocument":{"uri":"...","text":"..."}} */
    const char *uri_start = strstr(params, "\"uri\":\"");
    const char *text_start = strstr(params, "\"text\":\"");
    
    if (uri_start && text_start) {
        uri_start += 7;
        const char *uri_end = strchr(uri_start, '"');
        const char *text_end = strstr(text_start + 7, "\"}");
        
        if (uri_end && text_end) {
            char *uri = str_ndup(uri_start, uri_end - uri_start);
            char *text = str_ndup(text_start + 7, text_end - (text_start + 7));
            
            add_document(server, uri, text);
            
            LSPDocument *doc = find_document(server, uri);
            fprintf(stderr, "[LSP] Found doc: %p\n", (void*)doc);
            if (doc) {
                fprintf(stderr, "[LSP] Parsing symbols...\n");
                parse_document_symbols(doc);
                fprintf(stderr, "[LSP] Symbols parsed: %d symbols, %d imports\n", 
                        doc->symbol_count, doc->import_count);
                parse_diagnostics(doc);
                
                /* Send diagnostics */
                char diags_json[16384];
                char *p = diags_json;
                *p++ = '[';
                for (int i = 0; i < doc->diag_count; i++) {
                    if (i > 0) *p++ = ',';
                    /* Escape message for JSON */
                    char esc_msg[2048];
                    char *e = esc_msg;
                    const char *m = doc->diagnostics[i].message;
                    while (*m && e < esc_msg + sizeof(esc_msg) - 6) {
                        if (*m == '"') { *e++ = '\\'; *e++ = '"'; }
                        else if (*m == '\\') { *e++ = '\\'; *e++ = '\\'; }
                        else if (*m == '\n') { *e++ = '\\'; *e++ = 'n'; }
                        else if (*m == '\r') { *e++ = '\\'; *e++ = 'r'; }
                        else { *e++ = *m; }
                        m++;
                    }
                    *e = '\0';
                    
                    p += snprintf(p, 1024,
                        "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                        "\"end\":{\"line\":%d,\"character\":%d}},"
                        "\"severity\":%d,\"message\":\"%s\"}",
                        doc->diagnostics[i].line,
                        doc->diagnostics[i].col,
                        doc->diagnostics[i].end_line,
                        doc->diagnostics[i].end_col,
                        doc->diagnostics[i].severity,
                        esc_msg);
                }
                *p++ = ']';
                *p = '\0';
                
                char notif[17408];
                snprintf(notif, sizeof(notif),
                    "{\"uri\":\"%s\",\"diagnostics\":%s}", uri, diags_json);
                send_notification("textDocument/publishDiagnostics", notif);
            }
            
            free(uri);
            free(text);
        }
    }
}

static void handle_textDocument_didChange(LSPServer *server, const char *params) {
    /* Parse: {"textDocument":{"uri":"..."},"contentChanges":[{"text":"..."}]} */
    const char *uri_start = strstr(params, "\"uri\":\"");
    const char *changes_start = strstr(params, "\"text\":\"");
    
    if (uri_start && changes_start) {
        uri_start += 7;
        const char *uri_end = strchr(uri_start, '"');
        
        if (uri_end) {
            char *uri = str_ndup(uri_start, uri_end - uri_start);
            
            /* Find the last text field (most recent change) */
            const char *last_text = str_rstr(changes_start, "\"text\":\"");
            if (last_text) {
                last_text += 7;
                const char *text_end = strstr(last_text, "\"}");
                if (text_end) {
                    char *text = str_ndup(last_text, text_end - last_text);
                    update_document(server, uri, text);
                    
                    LSPDocument *doc = find_document(server, uri);
                    if (doc) {
                        parse_document_symbols(doc);
                        parse_diagnostics(doc);
                        
                        /* Send diagnostics */
                        char diags_json[16384];
                        char *p = diags_json;
                        *p++ = '[';
                        for (int i = 0; i < doc->diag_count; i++) {
                            if (i > 0) *p++ = ',';
                            p += snprintf(p, 1024,
                                "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                                "\"end\":{\"line\":%d,\"character\":%d}},"
                                "\"severity\":%d,\"message\":\"%s\"}",
                                doc->diagnostics[i].line,
                                doc->diagnostics[i].col,
                                doc->diagnostics[i].end_line,
                                doc->diagnostics[i].end_col,
                                doc->diagnostics[i].severity,
                                doc->diagnostics[i].message);
                        }
                        *p++ = ']';
                        *p = '\0';
                        
                        char notif[17408];
                        snprintf(notif, sizeof(notif),
                            "{\"uri\":\"%s\",\"diagnostics\":%s}", uri, diags_json);
                        send_notification("textDocument/publishDiagnostics", notif);
                    }
                    
                    free(text);
                }
            }
            
            free(uri);
        }
    }
}

static void handle_textDocument_didClose(LSPServer *server, const char *params) {
    const char *uri_start = strstr(params, "\"uri\":\"");
    if (uri_start) {
        uri_start += 7;
        const char *uri_end = strchr(uri_start, '"');
        if (uri_end) {
            char *uri = str_ndup(uri_start, uri_end - uri_start);
            remove_document(server, uri);
            free(uri);
        }
    }
}

static void handle_completion(LSPServer *server, int64_t id, const char *params) {
    fprintf(stderr, "[LSP] handle_completion called\n");
    /* Parse position and uri, find completions */
    const char *uri_start = strstr(params, "\"uri\":\"");
    const char *line_start = strstr(params, "\"line\":");
    const char *char_start = strstr(params, "\"character\":");
    
    if (!uri_start || !line_start || !char_start) {
        send_response_with_result(id, "[]");
        return;
    }
    
    uri_start += 7;
    const char *uri_end = strchr(uri_start, '"');
    if (!uri_end) {
        send_response_with_result(id, "[]");
        return;
    }
    
    char *uri = str_ndup(uri_start, uri_end - uri_start);
    LSPDocument *doc = find_document(server, uri);
    free(uri);
    
    if (!doc) {
        send_response_with_result(id, "[]");
        return;
    }
    
    /* Build completions based on current context */
    char json[8192];
    char *p = json;
    *p++ = '[';
    
    /* Keywords */
    const char *keywords[] = {
        "let", "var", "set", "if", "elsif", "else", "for", "in",
        "match", "case", "default", "defcomp", "macro", "import",
        "export", "output", "children", "dynamic", "single",
        "interface", "slots", "style", "script", "fill"
    };
    
    for (size_t i = 0; i < sizeof(keywords)/sizeof(keywords[0]); i++) {
        if (i > 0) *p++ = ',';
        p += snprintf(p, 256, "{\"label\":\"%s\",\"kind\":14,\"insertText\":\"%s\"}",
            keywords[i], keywords[i]);
    }
    
    /* HTML tags */
    const char *tags[] = {
        "html", "head", "body", "div", "span", "p", "a", "img",
        "ul", "ol", "li", "table", "tr", "td", "th", "form",
        "input", "button", "select", "option", "h1", "h2", "h3",
        "header", "footer", "nav", "main", "section", "article"
    };
    
    for (size_t i = 0; i < sizeof(tags)/sizeof(tags[0]); i++) {
        *p++ = ',';
        p += snprintf(p, 256, "{\"label\":\"%s\",\"kind\":7,\"insertText\":\"%s\"}",
            tags[i], tags[i]);
    }
    
    /* Builtins */
    const char *builtins[] = {
        "uppercase", "lowercase", "trim", "length", "formatCurrency",
        "formatDate", "default", "json", "split", "join", "replace"
    };
    
    for (size_t i = 0; i < sizeof(builtins)/sizeof(builtins[0]); i++) {
        *p++ = ',';
        p += snprintf(p, 256, "{\"label\":\"%s\",\"kind\":3,\"insertText\":\"%s\"}",
            builtins[i], builtins[i]);
    }
    
    /* Local symbols */
    for (int i = 0; i < doc->symbol_count; i++) {
        *p++ = ',';
        p += snprintf(p, 256, "{\"label\":\"%s\",\"kind\":%d,\"insertText\":\"%s\"}",
            doc->symbols[i].name, doc->symbols[i].kind, doc->symbols[i].name);
    }
    
    *p++ = ']';
    *p = '\0';
    
    send_response_with_result(id, json);
}

static void handle_hover(LSPServer *server, int64_t id, const char *params) {
    const char *uri_start = strstr(params, "\"uri\":\"");
    const char *line_start = strstr(params, "\"line\":");
    const char *char_start = strstr(params, "\"character\":");
    
    if (!uri_start || !line_start || !char_start) {
        send_response_with_result(id, "{\"contents\":\"\"}");
        return;
    }
    
    uri_start += 7;
    const char *uri_end = strchr(uri_start, '"');
    if (!uri_end) {
        send_response_with_result(id, "{\"contents\":\"\"}");
        return;
    }
    
    char *uri = str_ndup(uri_start, uri_end - uri_start);
    LSPDocument *doc = find_document(server, uri);
    free(uri);
    
    if (!doc) {
        send_response_with_result(id, "{\"contents\":\"\"}");
        return;
    }
    
    int line = atoi(line_start + 7);
    int col = atoi(char_start + 12);
    
    /* Find symbol at position */
    for (int i = 0; i < doc->symbol_count; i++) {
        if (doc->symbols[i].line == line && 
            doc->symbols[i].col <= col && 
            doc->symbols[i].end_col >= col) {
            
            const char *kind_name = "symbol";
            if (doc->symbols[i].kind == 7) kind_name = "component";
            else if (doc->symbols[i].kind == 14) kind_name = "macro";
            else if (doc->symbols[i].kind == 8) kind_name = "interface";
            else if (doc->symbols[i].kind == 6) kind_name = "variable";
            
            char json[1024];
            snprintf(json, sizeof(json),
                "{\"contents\":{\"kind\":\"markdown\",\"value\":\"**%s**\\n\\nA Motus %s\"}}",
                doc->symbols[i].name, kind_name);
            send_response_with_result(id, json);
            return;
        }
    }
    
    send_response_with_result(id, "{\"contents\":\"\"}");
}

static void handle_definition(LSPServer *server, int64_t id, const char *params) {
    fprintf(stderr, "[LSP] handle_definition called\n");
    const char *uri_start = strstr(params, "\"uri\":\"");
    const char *line_start = strstr(params, "\"line\":");
    const char *char_start = strstr(params, "\"character\":");
    
    if (!uri_start || !line_start || !char_start) {
        send_response_with_result(id, "null");
        return;
    }
    
    uri_start += 7;
    const char *uri_end = strchr(uri_start, '"');
    if (!uri_end) {
        send_response_with_result(id, "null");
        return;
    }
    
    char *uri = str_ndup(uri_start, uri_end - uri_start);
    fprintf(stderr, "[LSP] handle_definition: uri=%s\n", uri);
    LSPDocument *doc = find_document(server, uri);
    free(uri);
    
    if (!doc) {
        send_response_with_result(id, "null");
        return;
    }
    
    int line = atoi(line_start + 7);
    int col = atoi(char_start + 12);
    
    fprintf(stderr, "[LSP] handle_definition: line=%d, col=%d, symbols=%d, imports=%d\n", 
            line, col, doc->symbol_count, doc->import_count);
    
    for (int i = 0; i < doc->import_count; i++) {
        fprintf(stderr, "[LSP] import[%d]: %s -> %s at line %d, col %d-%d\n",
                i, doc->imports[i].local_name, doc->imports[i].import_path,
                doc->imports[i].line, doc->imports[i].col, doc->imports[i].end_col);
    }
    
    /* Find symbol at position */
    for (int i = 0; i < doc->symbol_count; i++) {
        if (doc->symbols[i].line == line && 
            doc->symbols[i].col <= col && 
            doc->symbols[i].end_col >= col) {
            
            char json[1024];
            snprintf(json, sizeof(json),
                "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                "\"end\":{\"line\":%d,\"character\":%d}}}",
                doc->uri,
                doc->symbols[i].line,
                doc->symbols[i].col,
                doc->symbols[i].end_line,
                doc->symbols[i].end_col);
            send_response_with_result(id, json);
            return;
        }
    }
    
    /* Check if cursor is on an imported component name */
    for (int i = 0; i < doc->import_count; i++) {
        if (doc->imports[i].line == line && 
            doc->imports[i].col <= col && 
            doc->imports[i].end_col >= col) {
            
            /* Found an import - try to find the imported file */
            char import_path[512];
            snprintf(import_path, sizeof(import_path), "%s.mot", doc->imports[i].import_path);
            
            /* Try to find the file relative to current file's directory */
            char base_dir[512] = {0};
            if (doc->file_path) {
                const char *last_slash = strrchr(doc->file_path, '/');
                if (last_slash) {
                    size_t len = last_slash - doc->file_path;
                    if (len < sizeof(base_dir) - 1) {
                        memcpy(base_dir, doc->file_path, len);
                        base_dir[len] = '\0';
                    }
                }
            }
            
            /* Try multiple paths */
            char full_path[1024];
            const char *search_paths[] = {
                base_dir,
                ".",
                "example/content",
                "example/content/components",
                "example/content/pages"
            };
            
            FILE *import_file = NULL;
            for (size_t s = 0; s < sizeof(search_paths)/sizeof(search_paths[0]); s++) {
                if (search_paths[s][0] == '\0') {
                    snprintf(full_path, sizeof(full_path), "%s", import_path);
                } else {
                    snprintf(full_path, sizeof(full_path), "%s/%s", search_paths[s], import_path);
                }
                import_file = fopen(full_path, "r");
                if (import_file) break;
            }
            
            if (import_file) {
                /* Read the imported file and search for defcomp */
                char import_content[16384];
                size_t read_len = fread(import_content, 1, sizeof(import_content) - 1, import_file);
                import_content[read_len] = '\0';
                fclose(import_file);
                
                /* Find defcomp in the imported file */
                const char *defcomp = strstr(import_content, "<defcomp ");
                if (defcomp) {
                    const char *name = defcomp + 9;
                    while (*name == ' ' || *name == '|') name++;
                    const char *name_end = name;
                    while (*name_end && (isalnum(*name_end) || *name_end == '_')) name_end++;
                    
                    /* Calculate line number */
                    int defcomp_line = 0;
                    const char *p = import_content;
                    while (p < defcomp) {
                        if (*p == '\n') defcomp_line++;
                        p++;
                    }
                    
                    /* Build the imported file URI */
                    char imported_uri[1024];
                    snprintf(imported_uri, sizeof(imported_uri), "file://%s/%s", 
                             base_dir[0] ? base_dir : ".", import_path);
                    
                    char json[1024];
                    snprintf(json, sizeof(json),
                        "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                        "\"end\":{\"line\":%d,\"character\":%d}}}",
                        imported_uri,
                        defcomp_line,
                        defcomp_line,
                        (int)(name_end - name));
                    send_response_with_result(id, json);
                    return;
                }
            }
            
            /* Couldn't find the imported file */
            char json[512];
            snprintf(json, sizeof(json),
                "{\"contents\":{\"kind\":\"markdown\",\"value\":\"Imported from: %s\\n(File not found)\"}}",
                doc->imports[i].import_path);
            send_response_with_result(id, json);
            return;
        }
    }
    
    send_response_with_result(id, "null");
}

static void handle_documentSymbol(LSPServer *server, int64_t id, const char *params) {
    const char *uri_start = strstr(params, "\"uri\":\"");
    if (!uri_start) {
        send_response_with_result(id, "[]");
        return;
    }
    
    uri_start += 7;
    const char *uri_end = strchr(uri_start, '"');
    if (!uri_end) {
        send_response_with_result(id, "[]");
        return;
    }
    
    char *uri = str_ndup(uri_start, uri_end - uri_start);
    LSPDocument *doc = find_document(server, uri);
    free(uri);
    
    if (!doc) {
        send_response_with_result(id, "[]");
        return;
    }
    
    char json[16384];
    char *p = json;
    *p++ = '[';
    
    for (int i = 0; i < doc->symbol_count; i++) {
        if (i > 0) *p++ = ',';
        
        const char *kind_name = "Symbol";
        if (doc->symbols[i].kind == 7) kind_name = "Component";
        else if (doc->symbols[i].kind == 14) kind_name = "Macro";
        else if (doc->symbols[i].kind == 8) kind_name = "Interface";
        else if (doc->symbols[i].kind == 6) kind_name = "Variable";
        
        p += snprintf(p, 1024,
            "{\"name\":\"%s\",\"kind\":%d,\"location\":{\"uri\":\"%s\","
            "\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
            "\"end\":{\"line\":%d,\"character\":%d}}},"
            "\"containerName\":\"%s\"}",
            doc->symbols[i].name,
            doc->symbols[i].kind,
            doc->uri,
            doc->symbols[i].line,
            doc->symbols[i].col,
            doc->symbols[i].end_line,
            doc->symbols[i].end_col,
            kind_name);
    }
    
    *p++ = ']';
    *p = '\0';
    
    send_response_with_result(id, json);
}

/* ============================================================================
 * Message Dispatch
 * ============================================================================ */

static void dispatch_message(LSPServer *server, const char *json) {
    /* Parse JSON-RPC */
    const char *id_str = strstr(json, "\"id\":");
    const char *method_start = strstr(json, "\"method\":\"");
    const char *params_start = strstr(json, "\"params\":");
    
    if (!method_start) return;
    
    method_start += 10;
    const char *method_end = strchr(method_start, '"');
    if (!method_end) return;
    
    char *method = str_ndup(method_start, method_end - method_start);
    
    int64_t id = -1;
    if (id_str) {
        id = atoll(id_str + 5);
    }
    
    const char *params = "";
    if (params_start) {
        params = params_start + 8;
    }
    
    /* Route to handler */
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(server, id, params);
    } else if (strcmp(method, "initialized") == 0) {
        handle_initialized(server, params);
    } else if (strcmp(method, "shutdown") == 0) {
        handle_shutdown(server, id, params);
    } else if (strcmp(method, "exit") == 0) {
        handle_exit(server, params);
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_textDocument_didOpen(server, params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_textDocument_didChange(server, params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_textDocument_didClose(server, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(server, id, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(server, id, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(server, id, params);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_documentSymbol(server, id, params);
    } else if (id >= 0) {
        send_error_response(id, -32601, "Method not found");
    }
    
    free(method);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    LSPServer server = {0};
    
    /* Read messages from stdin */
    char buffer[MAX_MESSAGE_SIZE + 1];
    size_t content_length = 0;
    
    while (1) {
        /* Read headers */
        content_length = 0;
        char line[MAX_LINE_LENGTH];
        
        /* Check for EOF */
        if (feof(stdin)) break;
        
        while (fgets(line, sizeof(line), stdin)) {
            if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
                break; /* End of headers */
            }
            if (strncmp(line, "Content-Length:", 15) == 0) {
                content_length = atoi(line + 15);
            }
        }
        
        if (content_length == 0) {
            if (feof(stdin)) break;
            continue;
        }
        
        /* Read content */
        size_t total_read = 0;
        while (total_read < content_length) {
            size_t n = fread(buffer + total_read, 1, content_length - total_read, stdin);
            if (n == 0) {
                if (feof(stdin)) break;
                break;
            }
            total_read += n;
        }
        
        if (total_read > 0) {
            buffer[total_read] = '\0';
            dispatch_message(&server, buffer);
        }
    }
    
    return 0;
}
