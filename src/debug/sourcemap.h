/*
 * Source map helpers (VLQ mappings + JSON serialization)
 */

#ifndef MOT_SOURCEMAP_H
#define MOT_SOURCEMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool ok;

    int32_t current_generated_line;
    int32_t last_generated_col;
    int32_t last_source;
    int32_t last_source_line;
    int32_t last_source_col;
    int32_t last_name;
    bool has_segment_on_line;
} SourceMapBuilder;

/* Encode one signed integer using Source Map Base64 VLQ. */
bool sourcemap_encode_vlq_to_buffer(int32_t value, char *out, size_t out_cap, size_t *out_len);

/* Build the "mappings" field incrementally. */
void sourcemap_builder_init(SourceMapBuilder *b);
void sourcemap_builder_free(SourceMapBuilder *b);
bool sourcemap_builder_add_mapping(SourceMapBuilder *b,
                                   int32_t generated_line,
                                   int32_t generated_col,
                                   int32_t source_index,
                                   int32_t source_line,
                                   int32_t source_col,
                                   int32_t name_index);
const char *sourcemap_builder_mappings(const SourceMapBuilder *b);

/*
 * Serialize a complete v3 source map JSON payload.
 * Returned string is heap allocated and must be freed by caller.
 */
char *sourcemap_render_json(const char *file,
                            const char *const *sources,
                            size_t source_count,
                            const char *const *names,
                            size_t name_count,
                            const char *mappings);

#endif /* MOT_SOURCEMAP_H */
