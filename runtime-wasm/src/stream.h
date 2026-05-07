/*
 * Streaming Output for WASM Runtime
 *
 * Handles buffered output and HTML escaping.
 */

#ifndef MOT_WASM_STREAM_H
#define MOT_WASM_STREAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define STREAM_BUFFER_SIZE 4096

typedef struct {
    char buffer[STREAM_BUFFER_SIZE];
    uint32_t pos;
} StreamBuffer;

/* Initialize stream buffer */
void stream_init(StreamBuffer *stream);

/* Flush buffer to host */
void stream_flush(StreamBuffer *stream);

/* Write raw data (unescaped) */
void stream_write_raw(StreamBuffer *stream, const char *data, uint32_t len);

/* Write text with HTML escaping */
void stream_write_text(StreamBuffer *stream, const char *data, uint32_t len);

/* Write a single character */
void stream_write_char(StreamBuffer *stream, char c);

/* Write a C string (null-terminated) */
void stream_write_str(StreamBuffer *stream, const char *str);

/* Write an integer */
void stream_write_int(StreamBuffer *stream, int64_t value);

/* Write a float */
void stream_write_float(StreamBuffer *stream, double value);

#endif /* MOT_WASM_STREAM_H */
