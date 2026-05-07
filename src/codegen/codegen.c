/*
 * Code Generation - Main extraction logic
 */

#include "codegen.h"
#include <string.h>
#include <stdio.h>

/* Current component context during extraction */
typedef struct {
    Arena *arena;
    const char *current_component;
    CSSBlock **css_tail;
    JSBlock **js_tail;
    int css_count;
    int js_count;
} ExtractContext;

/* Forward declaration */
static void extract_from_node(ExtractContext *ctx, AstNode *node);

/* Add a CSS block */
static void add_css_block(ExtractContext *ctx, const char *content, size_t length) {
    CSSBlock *block = arena_alloc(ctx->arena, sizeof(CSSBlock));

    /* Scope the CSS if we're inside a component */
    if (ctx->current_component) {
        block->content = css_scope(ctx->arena, content, length, ctx->current_component);
        block->length = strlen(block->content);
    } else {
        /* Global CSS - copy as-is */
        char *copy = arena_alloc(ctx->arena, length + 1);
        memcpy(copy, content, length);
        copy[length] = '\0';
        block->content = copy;
        block->length = length;
    }

    block->component = ctx->current_component;
    block->next = NULL;

    *ctx->css_tail = block;
    ctx->css_tail = &block->next;
    ctx->css_count++;
}

/* Add a JS block */
static void add_js_block(ExtractContext *ctx, const char *content, size_t length) {
    JSBlock *block = arena_alloc(ctx->arena, sizeof(JSBlock));

    /* Copy the JS content */
    char *copy = arena_alloc(ctx->arena, length + 1);
    memcpy(copy, content, length);
    copy[length] = '\0';

    block->content = copy;
    block->length = length;
    block->component = ctx->current_component;
    block->next = NULL;

    *ctx->js_tail = block;
    ctx->js_tail = &block->next;
    ctx->js_count++;
}

/* Extract from a list of nodes */
static void extract_from_children(ExtractContext *ctx, AstNode *node) {
    while (node) {
        extract_from_node(ctx, node);
        node = node->next;
    }
}

/* Extract from a single node */
static void extract_from_node(ExtractContext *ctx, AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_STYLE:
            add_css_block(ctx, node->data.embedded.code, node->data.embedded.code_len);
            break;

        case NODE_SCRIPT:
            add_js_block(ctx, node->data.embedded.code, node->data.embedded.code_len);
            break;

        case NODE_DEFCOMP: {
            /* Enter component context */
            const char *prev_component = ctx->current_component;
            ctx->current_component = node->data.defcomp.name;

            /* Extract from component body */
            extract_from_children(ctx, node->data.defcomp.body);

            /* Restore context */
            ctx->current_component = prev_component;
            break;
        }

        case NODE_DOCUMENT:
            extract_from_children(ctx, node->data.document.children);
            break;

        case NODE_ELEMENT:
            extract_from_children(ctx, node->data.element.children);
            break;

        case NODE_LET:
        case NODE_VAR:
            extract_from_children(ctx, node->data.binding.body);
            break;

        case NODE_IF:
            extract_from_children(ctx, node->data.if_stmt.then_body);
            if (node->data.if_stmt.else_branch) {
                extract_from_node(ctx, node->data.if_stmt.else_branch);
            }
            break;

        case NODE_ELSIF:
            extract_from_children(ctx, node->data.if_stmt.then_body);
            if (node->data.if_stmt.else_branch) {
                extract_from_node(ctx, node->data.if_stmt.else_branch);
            }
            break;

        case NODE_ELSE:
            extract_from_children(ctx, node->data.else_stmt.body);
            break;

        case NODE_FOR:
            extract_from_children(ctx, node->data.for_loop.body);
            break;

        case NODE_MACRO:
            extract_from_children(ctx, node->data.macro.body);
            break;

        default:
            /* Other nodes don't have nested content */
            break;
    }
}

/* Extract CSS and JS from an AST */
CodegenResult *codegen_extract(Arena *arena, AstNode *doc) {
    CodegenResult *result = arena_alloc(arena, sizeof(CodegenResult));
    result->css = NULL;
    result->js = NULL;
    result->css_count = 0;
    result->js_count = 0;

    ExtractContext ctx = {
        .arena = arena,
        .current_component = NULL,
        .css_tail = &result->css,
        .js_tail = &result->js,
        .css_count = 0,
        .js_count = 0
    };

    extract_from_node(&ctx, doc);

    result->css_count = ctx.css_count;
    result->js_count = ctx.js_count;

    return result;
}

/* Combine all CSS blocks into a single string */
char *codegen_combine_css(Arena *arena, CodegenResult *result) {
    if (!result || result->css_count == 0) {
        char *empty = arena_alloc(arena, 1);
        empty[0] = '\0';
        return empty;
    }

    /* Calculate total length */
    size_t total_len = 0;
    for (CSSBlock *block = result->css; block; block = block->next) {
        total_len += block->length + 2;  /* +2 for newlines between blocks */
    }

    /* Allocate and combine */
    char *combined = arena_alloc(arena, total_len + 1);
    size_t pos = 0;

    for (CSSBlock *block = result->css; block; block = block->next) {
        memcpy(combined + pos, block->content, block->length);
        pos += block->length;
        if (block->next) {
            combined[pos++] = '\n';
            combined[pos++] = '\n';
        }
    }

    combined[pos] = '\0';
    return combined;
}

/* Combine all JS blocks into a single string */
char *codegen_combine_js(Arena *arena, CodegenResult *result) {
    if (!result || result->js_count == 0) {
        char *empty = arena_alloc(arena, 1);
        empty[0] = '\0';
        return empty;
    }

    /* Calculate total length */
    size_t total_len = 0;
    for (JSBlock *block = result->js; block; block = block->next) {
        total_len += block->length + 2;
    }

    /* Allocate and combine */
    char *combined = arena_alloc(arena, total_len + 1);
    size_t pos = 0;

    for (JSBlock *block = result->js; block; block = block->next) {
        memcpy(combined + pos, block->content, block->length);
        pos += block->length;
        if (block->next) {
            combined[pos++] = '\n';
            combined[pos++] = '\n';
        }
    }

    combined[pos] = '\0';
    return combined;
}
