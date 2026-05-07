#ifndef MOT_FREESTANDING_STDIO_H
#define MOT_FREESTANDING_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define FILE void
#define stderr ((void*)0)
#define stdout ((void*)0)

int printf(const char *fmt, ...);
int fprintf(void *stream, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
size_t fwrite(const void *ptr, size_t size, size_t count, void *stream);

#endif
