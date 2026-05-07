/*
 * Streaming Output Implementation
 */

#include "stream.h"
#include "host.h"
#include <string.h>
#include <stdio.h>

void stream_init(StreamBuffer *stream) {
    stream->pos = 0;
}

void stream_flush(StreamBuffer *stream) {
    if (stream->pos > 0) {
        host_output(stream->buffer, stream->pos);
        stream->pos = 0;
    }
}

static void ensure_space(StreamBuffer *stream, uint32_t needed) {
    if (stream->pos + needed > STREAM_BUFFER_SIZE) {
        stream_flush(stream);
    }
}

void stream_write_raw(StreamBuffer *stream, const char *data, uint32_t len) {
    /* Handle large writes directly */
    if (len > STREAM_BUFFER_SIZE / 2) {
        stream_flush(stream);
        host_output(data, len);
        return;
    }

    ensure_space(stream, len);
    memcpy(stream->buffer + stream->pos, data, len);
    stream->pos += len;
}

void stream_write_text(StreamBuffer *stream, const char *data, uint32_t len) {
    /* Escape HTML special characters */
    for (uint32_t i = 0; i < len; i++) {
        char c = data[i];
        switch (c) {
            case '<':
                stream_write_raw(stream, "&lt;", 4);
                break;
            case '>':
                stream_write_raw(stream, "&gt;", 4);
                break;
            case '&':
                stream_write_raw(stream, "&amp;", 5);
                break;
            case '"':
                stream_write_raw(stream, "&quot;", 6);
                break;
            case '\'':
                stream_write_raw(stream, "&#39;", 5);
                break;
            default:
                stream_write_char(stream, c);
                break;
        }
    }
}

void stream_write_char(StreamBuffer *stream, char c) {
    ensure_space(stream, 1);
    stream->buffer[stream->pos++] = c;
}

void stream_write_str(StreamBuffer *stream, const char *str) {
    stream_write_raw(stream, str, (uint32_t)strlen(str));
}

void stream_write_int(StreamBuffer *stream, int64_t value) {
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    stream_write_raw(stream, buf, (uint32_t)len);
}

void stream_write_float(StreamBuffer *stream, double value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%g", value);
    stream_write_raw(stream, buf, (uint32_t)len);
}
